#define _GNU_SOURCE

#include <asm/ptrace.h>
#include <elf.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../include/drm-uapi/mali_kbase_ioctl.h"

#define MAX_ALLOCS 4096
#define MAX_MAPPINGS 4096
#define MAX_THREADS 4096
#define MAX_CHAIN_JOBS 256
#define SNAPSHOT_BYTES 384

struct allocation {
   uint64_t key;
   uint64_t gpu;
   uint64_t pages;
   uint64_t flags;
   uint32_t id;
   bool active;
   bool awaiting_mapping;
};

struct mapping {
   uintptr_t cpu;
   uint64_t gpu;
   size_t length;
   uint32_t allocation_id;
   bool active;
};

struct thread_state {
   pid_t tid;
   bool active;
   bool entering;
   long syscall_number;
   uint64_t arguments[6];
   union kbase_ioctl_mem_alloc alloc_before;
   union kbase_ioctl_mem_import import_before;
   struct kbase_ioctl_mem_free free_before;
};

static struct allocation allocations[MAX_ALLOCS];
static struct mapping mappings[MAX_MAPPINGS];
static struct thread_state threads[MAX_THREADS];
static uint32_t next_allocation_id = 1;
static uint64_t event_sequence = 1;
static uint64_t submit_sequence = 1;
static FILE *trace_file;
static const char *trace_label;
static int last_process_vm_error;
static int last_ptrace_error;

static uint64_t
monotonic_ns(void)
{
   struct timespec time;
   clock_gettime(CLOCK_MONOTONIC, &time);
   return (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
}

static void
emit(const char *format, ...)
{
   va_list arguments;
   va_start(arguments, format);
   vfprintf(trace_file, format, arguments);
   va_end(arguments);
   fputc('\n', trace_file);
   fflush(trace_file);
}

static bool
read_process(pid_t tid, uint64_t address, void *destination, size_t length)
{
   last_process_vm_error = 0;
   last_ptrace_error = 0;
   struct iovec local = {.iov_base = destination, .iov_len = length};
   struct iovec remote = {
      .iov_base = (void *)(uintptr_t)address,
      .iov_len = length,
   };
   ssize_t read_count = process_vm_readv(tid, &local, 1, &remote, 1, 0);
   if (read_count == (ssize_t)length)
      return true;
   last_process_vm_error = read_count < 0 ? errno : 0;

   size_t copied = 0;
   while (copied < length) {
      errno = 0;
      long word = ptrace(PTRACE_PEEKDATA, tid,
                         (void *)(uintptr_t)(address + copied), NULL);
      if (word == -1 && errno) {
         last_ptrace_error = errno;
         return false;
      }
      size_t amount = sizeof(word);
      if (amount > length - copied)
         amount = length - copied;
      memcpy((uint8_t *)destination + copied, &word, amount);
      copied += amount;
   }
   return true;
}

static bool
is_kbase_fd(pid_t tid, int fd)
{
   char path[64];
   char target[512];
   snprintf(path, sizeof(path), "/proc/%d/fd/%d", tid, fd);
   ssize_t length = readlink(path, target, sizeof(target) - 1);
   if (length <= 0)
      return false;
   target[length] = '\0';
   return strstr(target, "/dev/mali") != NULL;
}

static struct thread_state *
thread_state(pid_t tid)
{
   for (size_t i = 0; i < MAX_THREADS; ++i) {
      if (threads[i].active && threads[i].tid == tid)
         return &threads[i];
   }
   for (size_t i = 0; i < MAX_THREADS; ++i) {
      if (!threads[i].active) {
         memset(&threads[i], 0, sizeof(threads[i]));
         threads[i].tid = tid;
         threads[i].active = true;
         threads[i].entering = true;
         return &threads[i];
      }
   }
   return NULL;
}

static struct allocation *
find_gpu_allocation(uint64_t gpu)
{
   for (size_t i = 0; i < MAX_ALLOCS; ++i) {
      if (allocations[i].active && allocations[i].gpu == gpu)
         return &allocations[i];
   }
   return NULL;
}

static struct allocation *
find_pending_allocation(uint64_t key)
{
   struct allocation *latest = NULL;
   for (size_t i = 0; i < MAX_ALLOCS; ++i) {
      if (allocations[i].active && allocations[i].awaiting_mapping &&
          allocations[i].key == key &&
          (!latest || allocations[i].id > latest->id))
         latest = &allocations[i];
   }
   return latest;
}

static struct allocation *
record_allocation(uint64_t key, uint64_t gpu, uint64_t pages, uint64_t flags)
{
   struct allocation *slot = NULL;
   for (size_t i = 0; i < MAX_ALLOCS; ++i) {
      if (!allocations[i].active) {
         slot = &allocations[i];
         break;
      }
   }
   if (!slot)
      return NULL;
   slot->key = key;
   slot->gpu = gpu;
   slot->pages = pages;
   slot->flags = flags;
   slot->id = next_allocation_id++;
   slot->active = true;
   slot->awaiting_mapping = true;
   return slot;
}

static void
record_mapping(pid_t tid, uintptr_t cpu, size_t length, uint64_t offset)
{
   struct allocation *allocation = find_pending_allocation(offset);
   if (!allocation)
      return;
   allocation->awaiting_mapping = false;
   if (allocation->flags & BASE_MEM_SAME_VA)
      allocation->gpu = cpu;
   for (size_t i = 0; i < MAX_MAPPINGS; ++i) {
      if (!mappings[i].active) {
         mappings[i].cpu = cpu;
         mappings[i].gpu =
            (allocation->flags & BASE_MEM_SAME_VA) ? cpu : allocation->gpu;
         mappings[i].length = length;
         mappings[i].allocation_id = allocation->id;
         mappings[i].active = true;
         emit("{\"schema\":1,\"type\":\"mapping\",\"seq\":%" PRIu64
              ",\"time_ns\":%" PRIu64 ",\"pid\":%d,\"tid\":%d,"
              "\"allocation\":%u,\"gpu\":\"0x%" PRIx64
              "\",\"length\":%zu}",
              event_sequence++, monotonic_ns(), tid, tid, allocation->id,
              mappings[i].gpu, length);
         return;
      }
   }
}

static void
forget_mapping(pid_t tid, uintptr_t cpu, size_t length)
{
   for (size_t i = 0; i < MAX_MAPPINGS; ++i) {
      uintptr_t start = mappings[i].cpu;
      uintptr_t end = start + mappings[i].length;
      uintptr_t unmap_end = cpu + length;
      if (mappings[i].active && cpu < end && unmap_end > start) {
         emit("{\"schema\":1,\"type\":\"unmapping\",\"seq\":%" PRIu64
              ",\"time_ns\":%" PRIu64 ",\"pid\":%d,\"tid\":%d,"
              "\"allocation\":%u,\"cpu\":\"0x%" PRIxPTR
              "\",\"length\":%zu}",
              event_sequence++, monotonic_ns(), tid, tid,
              mappings[i].allocation_id, cpu, length);
         mappings[i].active = false;
      }
   }
}

static bool
copy_gpu(pid_t tid, uint64_t gpu, void *destination, size_t requested,
         uint32_t *allocation_id, uint64_t *offset, size_t *copied_length)
{
   for (size_t i = 0; i < MAX_MAPPINGS; ++i) {
      uint64_t start = mappings[i].gpu;
      uint64_t end = start + mappings[i].length;
      if (mappings[i].active && gpu >= start && gpu < end) {
         size_t available = (size_t)(end - gpu);
         size_t amount = requested < available ? requested : available;
         if (!read_process(tid, mappings[i].cpu + gpu - start, destination,
                           amount))
            return false;
         *allocation_id = mappings[i].allocation_id;
         *offset = gpu - start;
         *copied_length = amount;
         return true;
      }
   }
   return false;
}

static void
hex_encode(const uint8_t *bytes, size_t length, char *output)
{
   static const char hex[] = "0123456789abcdef";
   for (size_t i = 0; i < length; ++i) {
      output[i * 2] = hex[bytes[i] >> 4];
      output[i * 2 + 1] = hex[bytes[i] & 15];
   }
   output[length * 2] = '\0';
}

static void
capture_job_chain(pid_t tid, uint64_t submit_id, uint32_t atom_index,
                  uint64_t root)
{
   uint64_t address = root;
   uint64_t visited[MAX_CHAIN_JOBS];
   size_t visited_count = 0;
   uint8_t bytes[SNAPSHOT_BYTES];
   char encoded[SNAPSHOT_BYTES * 2 + 1];

   while (address && visited_count < MAX_CHAIN_JOBS) {
      bool cycle = false;
      for (size_t i = 0; i < visited_count; ++i)
         cycle |= visited[i] == address;
      if (cycle)
         break;
      visited[visited_count] = address;

      uint32_t allocation_id = 0;
      uint64_t offset = 0;
      size_t copied_length = 0;
      if (!copy_gpu(tid, address, bytes, sizeof(bytes), &allocation_id, &offset,
                    &copied_length) || copied_length < 32) {
         emit("{\"schema\":1,\"type\":\"job\",\"seq\":%" PRIu64
              ",\"time_ns\":%" PRIu64 ",\"pid\":%d,\"tid\":%d,"
              "\"submit\":%" PRIu64 ",\"atom_index\":%u,"
              "\"chain_index\":%zu,\"gpu\":\"0x%" PRIx64
              "\",\"mapped\":false,\"process_vm_errno\":%d,"
              "\"ptrace_errno\":%d}",
              event_sequence++, monotonic_ns(), tid, tid, submit_id,
              atom_index, visited_count, address, last_process_vm_error,
              last_ptrace_error);
         break;
      }

      uint32_t word4;
      uint32_t word5;
      uint64_t next;
      memcpy(&word4, bytes + 16, sizeof(word4));
      memcpy(&word5, bytes + 20, sizeof(word5));
      memcpy(&next, bytes + 24, sizeof(next));
      hex_encode(bytes, copied_length, encoded);
      emit("{\"schema\":1,\"type\":\"job\",\"seq\":%" PRIu64
           ",\"time_ns\":%" PRIu64 ",\"pid\":%d,\"tid\":%d,"
           "\"submit\":%" PRIu64 ",\"atom_index\":%u,"
           "\"chain_index\":%zu,\"allocation\":%u,\"offset\":%" PRIu64
           ",\"type_id\":%u,\"barrier\":%s,\"index\":%u,"
           "\"dependency_1\":%u,\"dependency_2\":%u,"
           "\"next_gpu\":\"0x%" PRIx64 "\",\"bytes\":\"%s\"}",
           event_sequence++, monotonic_ns(), tid, tid, submit_id, atom_index,
           visited_count, allocation_id, offset, (word4 >> 1) & 0x7f,
           (word4 & (1u << 8)) ? "true" : "false", word4 >> 16,
           word5 & 0xffff, word5 >> 16, next, encoded);
      address = next;
      visited_count++;
   }
}

static void
capture_submit(pid_t tid, uint64_t argument)
{
   struct kbase_ioctl_job_submit submit;
   if (!read_process(tid, argument, &submit, sizeof(submit)))
      return;
   uint64_t submit_id = submit_sequence++;
   emit("{\"schema\":1,\"type\":\"submit\",\"seq\":%" PRIu64
        ",\"time_ns\":%" PRIu64 ",\"pid\":%d,\"tid\":%d,"
        "\"submit\":%" PRIu64 ",\"nr_atoms\":%u,\"stride\":%u}",
        event_sequence++, monotonic_ns(), tid, tid, submit_id, submit.nr_atoms,
        submit.stride);
   if (submit.nr_atoms > 4096 || submit.stride < sizeof(base_jd_atom))
      return;

   for (uint32_t i = 0; i < submit.nr_atoms; ++i) {
      base_jd_atom atom;
      if (!read_process(tid, submit.addr + (uint64_t)i * submit.stride, &atom,
                        sizeof(atom)))
         break;
      emit("{\"schema\":1,\"type\":\"atom\",\"seq\":%" PRIu64
           ",\"time_ns\":%" PRIu64 ",\"pid\":%d,\"tid\":%d,"
           "\"submit\":%" PRIu64 ",\"atom_index\":%u,"
           "\"atom_number\":%u,\"core_req\":%u,\"jc\":\"0x%" PRIx64
           "\",\"pre_dep_0\":%u,\"pre_dep_type_0\":%u,"
           "\"pre_dep_1\":%u,\"pre_dep_type_1\":%u,\"priority\":%u,"
           "\"device\":%u,\"jobslot\":%u,\"seq_nr\":%" PRIu64 "}",
           event_sequence++, monotonic_ns(), tid, tid, submit_id, i,
           atom.atom_number, atom.core_req, atom.jc, atom.pre_dep[0].atom_id,
           atom.pre_dep[0].dependency_type, atom.pre_dep[1].atom_id,
           atom.pre_dep[1].dependency_type, atom.prio, atom.device_nr,
           atom.jobslot, atom.seq_nr);
      if (atom.jc && !(atom.core_req & BASE_JD_REQ_SOFT_JOB))
         capture_job_chain(tid, submit_id, i, atom.jc);
   }
}

static void
capture_events(pid_t tid, uint64_t address, size_t length)
{
   if (!length || length > 4096 || length % sizeof(struct base_jd_event_v2))
      return;
   struct base_jd_event_v2 events[4096 / sizeof(struct base_jd_event_v2)];
   if (!read_process(tid, address, events, length))
      return;
   size_t count = length / sizeof(events[0]);
   for (size_t i = 0; i < count; ++i) {
      emit("{\"schema\":1,\"type\":\"completion\",\"seq\":%" PRIu64
           ",\"time_ns\":%" PRIu64 ",\"pid\":%d,\"tid\":%d,"
           "\"event_code\":%u,\"atom_number\":%u,"
           "\"udata_0\":\"0x%" PRIx64 "\","
           "\"udata_1\":\"0x%" PRIx64 "\"}",
           event_sequence++, monotonic_ns(), tid, tid, events[i].event_code,
           events[i].atom_number, events[i].udata.blob[0],
           events[i].udata.blob[1]);
   }
}

static const char *
ioctl_name(unsigned long request)
{
   if (request == KBASE_IOCTL_VERSION_CHECK_JM)
      return "VERSION_CHECK_JM";
   if (request == KBASE_IOCTL_VERSION_CHECK_CSF)
      return "VERSION_CHECK_CSF";
   if (request == KBASE_IOCTL_SET_FLAGS)
      return "SET_FLAGS";
   if (request == KBASE_IOCTL_JOB_SUBMIT)
      return "JOB_SUBMIT";
   if (request == KBASE_IOCTL_GET_GPUPROPS)
      return "GET_GPUPROPS";
   if (request == KBASE_IOCTL_MEM_ALLOC)
      return "MEM_ALLOC";
   if (request == KBASE_IOCTL_MEM_QUERY)
      return "MEM_QUERY";
   if (request == KBASE_IOCTL_MEM_FREE)
      return "MEM_FREE";
   if (request == KBASE_IOCTL_MEM_SYNC)
      return "MEM_SYNC";
   if (request == KBASE_IOCTL_MEM_COMMIT)
      return "MEM_COMMIT";
   if (request == KBASE_IOCTL_MEM_ALIAS)
      return "MEM_ALIAS";
   if (request == KBASE_IOCTL_MEM_IMPORT)
      return "MEM_IMPORT";
   if (request == KBASE_IOCTL_MEM_EXEC_INIT)
      return "MEM_EXEC_INIT";
   if (_IOC_TYPE(request) == KBASE_IOCTL_TYPE) {
      switch (_IOC_NR(request)) {
      case 4:
         return "POST_TERM";
      case 14:
         return "MEM_JIT_INIT";
      case 24:
         return "STREAM_CREATE";
      case 55:
         return "SET_LIMITED_CORE_COUNT";
      }
   }
   return "UNKNOWN";
}

static bool
get_registers(pid_t tid, struct user_pt_regs *registers)
{
   struct iovec io = {.iov_base = registers, .iov_len = sizeof(*registers)};
   return ptrace(PTRACE_GETREGSET, tid, (void *)NT_PRSTATUS, &io) == 0;
}

static void
syscall_entry(struct thread_state *state, const struct user_pt_regs *registers)
{
   state->syscall_number = (long)registers->regs[8];
   for (size_t i = 0; i < 6; ++i)
      state->arguments[i] = registers->regs[i];

   if (state->syscall_number != __NR_ioctl)
      return;
   int fd = (int)state->arguments[0];
   unsigned long request = (unsigned long)state->arguments[1];
   uint64_t argument = state->arguments[2];
   if (!is_kbase_fd(state->tid, fd))
      return;
   if (request == KBASE_IOCTL_MEM_ALLOC)
      read_process(state->tid, argument, &state->alloc_before,
                   sizeof(state->alloc_before));
   if (request == KBASE_IOCTL_MEM_IMPORT)
      read_process(state->tid, argument, &state->import_before,
                   sizeof(state->import_before));
   if (request == KBASE_IOCTL_MEM_FREE)
      read_process(state->tid, argument, &state->free_before,
                   sizeof(state->free_before));
   if (request == KBASE_IOCTL_JOB_SUBMIT)
      capture_submit(state->tid, argument);
}

static void
syscall_exit(struct thread_state *state, const struct user_pt_regs *registers)
{
   int64_t result = (int64_t)registers->regs[0];
   if (state->syscall_number == __NR_read) {
      int fd = (int)state->arguments[0];
      if (result > 0 && is_kbase_fd(state->tid, fd))
         capture_events(state->tid, state->arguments[1], (size_t)result);
      return;
   }
   if (state->syscall_number == __NR_mmap) {
      int fd = (int)state->arguments[4];
      if (result >= 0 && is_kbase_fd(state->tid, fd))
         record_mapping(state->tid, (uintptr_t)result,
                        (size_t)state->arguments[1], state->arguments[5]);
      return;
   }
   if (state->syscall_number == __NR_munmap) {
      if (result == 0)
         forget_mapping(state->tid, (uintptr_t)state->arguments[0],
                        (size_t)state->arguments[1]);
      return;
   }
   if (state->syscall_number != __NR_ioctl)
      return;

   int fd = (int)state->arguments[0];
   unsigned long request = (unsigned long)state->arguments[1];
   uint64_t argument = state->arguments[2];
   if (!is_kbase_fd(state->tid, fd))
      return;
   int error = result < 0 && result >= -4095 ? (int)-result : 0;
   emit("{\"schema\":1,\"type\":\"ioctl\",\"seq\":%" PRIu64
        ",\"time_ns\":%" PRIu64 ",\"pid\":%d,\"tid\":%d,"
        "\"name\":\"%s\",\"request\":\"0x%lx\",\"number\":%lu,"
        "\"direction\":%lu,\"size\":%lu,\"result\":%" PRId64
        ",\"errno\":%d}",
        event_sequence++, monotonic_ns(), state->tid, state->tid,
        ioctl_name(request), request, (unsigned long)_IOC_NR(request),
        (unsigned long)_IOC_DIR(request), (unsigned long)_IOC_SIZE(request),
        result, error);

   if (result >= 0 && request == KBASE_IOCTL_MEM_ALLOC) {
      union kbase_ioctl_mem_alloc after;
      if (read_process(state->tid, argument, &after, sizeof(after))) {
         struct allocation *allocation =
            record_allocation(after.out.gpu_va, after.out.gpu_va,
                              state->alloc_before.in.va_pages, after.out.flags);
         emit("{\"schema\":1,\"type\":\"allocation\",\"seq\":%" PRIu64
              ",\"time_ns\":%" PRIu64 ",\"pid\":%d,\"tid\":%d,"
              "\"allocation\":%u,\"kind\":\"native\",\"pages\":%" PRIu64
              ",\"commit_pages\":%" PRIu64 ",\"flags\":\"0x%" PRIx64
              "\",\"gpu_or_cookie\":\"0x%" PRIx64 "\"}",
              event_sequence++, monotonic_ns(), state->tid, state->tid,
              allocation ? allocation->id : 0,
              state->alloc_before.in.va_pages,
              state->alloc_before.in.commit_pages, after.out.flags,
              after.out.gpu_va);
      }
   }
   if (result >= 0 && request == KBASE_IOCTL_MEM_IMPORT) {
      union kbase_ioctl_mem_import after;
      if (read_process(state->tid, argument, &after, sizeof(after))) {
         struct allocation *allocation =
            record_allocation(after.out.gpu_va, after.out.gpu_va,
                              after.out.va_pages, after.out.flags);
         emit("{\"schema\":1,\"type\":\"allocation\",\"seq\":%" PRIu64
              ",\"time_ns\":%" PRIu64 ",\"pid\":%d,\"tid\":%d,"
              "\"allocation\":%u,\"kind\":\"import\",\"pages\":%" PRIu64
              ",\"flags\":\"0x%" PRIx64 ",\"import_type\":%u,"
              "\"gpu_or_cookie\":\"0x%" PRIx64 "\"}",
              event_sequence++, monotonic_ns(), state->tid, state->tid,
              allocation ? allocation->id : 0, after.out.va_pages,
              after.out.flags, state->import_before.in.type, after.out.gpu_va);
      }
   }
   if (result >= 0 && request == KBASE_IOCTL_MEM_FREE) {
      struct allocation *allocation =
         find_gpu_allocation(state->free_before.gpu_addr);
      if (allocation)
         allocation->active = false;
   }
}

static int
trace_child(char **command)
{
   pid_t child = fork();
   if (child < 0) {
      perror("fork");
      return 1;
   }
   if (child == 0) {
      if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0) {
         perror("PTRACE_TRACEME");
         _exit(126);
      }
      raise(SIGSTOP);
      execvp(command[0], command);
      perror(command[0]);
      _exit(127);
   }

   int status;
   if (waitpid(child, &status, 0) < 0 || !WIFSTOPPED(status)) {
      fprintf(stderr, "child did not enter ptrace stop\n");
      return 1;
   }
   long options = PTRACE_O_TRACESYSGOOD | PTRACE_O_TRACECLONE |
                  PTRACE_O_TRACEFORK | PTRACE_O_TRACEVFORK |
                  PTRACE_O_TRACEEXEC | PTRACE_O_EXITKILL;
   if (ptrace(PTRACE_SETOPTIONS, child, NULL, (void *)options) < 0) {
      perror("PTRACE_SETOPTIONS");
      ptrace(PTRACE_DETACH, child, NULL, NULL);
      return 1;
   }
   thread_state(child);
   ptrace(PTRACE_SYSCALL, child, NULL, NULL);

   int exit_status = 0;
   while (true) {
      pid_t tid = waitpid(-1, &status, __WALL);
      if (tid < 0) {
         if (errno == EINTR)
            continue;
         if (errno == ECHILD)
            break;
         perror("waitpid");
         return 1;
      }

      struct thread_state *state = thread_state(tid);
      if (WIFEXITED(status)) {
         if (tid == child)
            exit_status = WEXITSTATUS(status);
         if (state)
            state->active = false;
         continue;
      }
      if (WIFSIGNALED(status)) {
         if (tid == child)
            exit_status = 128 + WTERMSIG(status);
         if (state)
            state->active = false;
         continue;
      }
      if (!WIFSTOPPED(status))
         continue;

      int signal = WSTOPSIG(status);
      unsigned int event = (unsigned int)status >> 16;
      if (signal == (SIGTRAP | 0x80) && state) {
         struct ptrace_syscall_info info;
         memset(&info, 0, sizeof(info));
         long info_size = ptrace(PTRACE_GET_SYSCALL_INFO, tid, sizeof(info),
                                 &info);
         if (info_size >= 0 && info.op == PTRACE_SYSCALL_INFO_ENTRY) {
            struct user_pt_regs registers;
            memset(&registers, 0, sizeof(registers));
            registers.regs[8] = info.entry.nr;
            for (size_t i = 0; i < 6; ++i)
               registers.regs[i] = info.entry.args[i];
            syscall_entry(state, &registers);
         } else if (info_size >= 0 && info.op == PTRACE_SYSCALL_INFO_EXIT) {
            struct user_pt_regs registers;
            memset(&registers, 0, sizeof(registers));
            registers.regs[0] = (uint64_t)info.exit.rval;
            syscall_exit(state, &registers);
         } else if (info_size < 0) {
            struct user_pt_regs registers;
            if (get_registers(tid, &registers)) {
               if (state->entering)
                  syscall_entry(state, &registers);
               else
                  syscall_exit(state, &registers);
               state->entering = !state->entering;
            }
         }
         ptrace(PTRACE_SYSCALL, tid, NULL, NULL);
      } else if (signal == SIGTRAP && event) {
         if (event == PTRACE_EVENT_CLONE || event == PTRACE_EVENT_FORK ||
             event == PTRACE_EVENT_VFORK) {
            unsigned long new_tid = 0;
            ptrace(PTRACE_GETEVENTMSG, tid, NULL, &new_tid);
            thread_state((pid_t)new_tid);
         }
         if (event == PTRACE_EVENT_EXEC && state)
            state->entering = false;
         ptrace(PTRACE_SYSCALL, tid, NULL, NULL);
      } else {
         int delivered = signal == SIGSTOP ? 0 : signal;
         ptrace(PTRACE_SYSCALL, tid, NULL, (void *)(intptr_t)delivered);
      }
   }
   return exit_status;
}

int
main(int argc, char **argv)
{
   if (argc < 5 || strcmp(argv[3], "--") != 0) {
      fprintf(stderr,
              "usage: %s output.jsonl label -- command [arguments...]\n",
              argv[0]);
      return 2;
   }
   trace_file = fopen(argv[1], "w");
   if (!trace_file) {
      perror(argv[1]);
      return 1;
   }
   trace_label = argv[2];
   emit("{\"schema\":1,\"type\":\"process\",\"seq\":%" PRIu64
        ",\"time_ns\":%" PRIu64 ",\"pid\":%d,\"tid\":%d,"
        "\"label\":\"%s\",\"snapshot_bytes\":%d,"
        "\"capture\":\"ptrace\"}",
        event_sequence++, monotonic_ns(), getpid(), getpid(), trace_label,
        SNAPSHOT_BYTES);
   int result = trace_child(&argv[4]);
   fclose(trace_file);
   return result;
}
