#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../../include/drm-uapi/mali_kbase_ioctl.h"

#define MAX_ALLOCS 4096
#define MAX_MAPPINGS 4096
#define MAX_CHAIN_JOBS 256
#define DEFAULT_SNAPSHOT_BYTES 384
#define MAX_SNAPSHOT_BYTES 4096

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

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t init_once = PTHREAD_ONCE_INIT;
static struct allocation allocations[MAX_ALLOCS];
static struct mapping mappings[MAX_MAPPINGS];
static atomic_uint_fast64_t event_sequence = 1;
static atomic_uint_fast64_t submit_sequence = 1;
static uint32_t next_allocation_id = 1;
static char trace_path[4096];
static char trace_label[256];
static size_t snapshot_bytes = DEFAULT_SNAPSHOT_BYTES;
static bool trace_enabled;

static void write_all(int fd, const char *data, size_t length);

static void
emit_error(const char *message, int error)
{
   char line[1024];
   int length = snprintf(line, sizeof(line), "kbase-oracle: %s: %s\n", message,
                         strerror(error));
   if (length > 0)
      write_all(STDERR_FILENO, line,
                (size_t)length < sizeof(line) ? (size_t)length
                                              : sizeof(line) - 1);
}

static uint64_t
monotonic_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static long
thread_id(void)
{
   return syscall(SYS_gettid);
}

static void
json_escape(const char *src, char *dst, size_t dst_size)
{
   size_t out = 0;
   for (size_t i = 0; src && src[i] && out + 2 < dst_size; ++i) {
      unsigned char c = (unsigned char)src[i];
      if (c == '"' || c == '\\') {
         dst[out++] = '\\';
         dst[out++] = (char)c;
      } else if (c >= 0x20) {
         dst[out++] = (char)c;
      }
   }
   dst[out] = '\0';
}

static void
write_all(int fd, const char *data, size_t length)
{
   while (length) {
      ssize_t written = syscall(SYS_write, fd, data, length);
      if (written < 0) {
         if (errno == EINTR)
            continue;
         return;
      }
      data += written;
      length -= (size_t)written;
   }
}

static void
emit_line(const char *format, ...)
{
   if (!trace_enabled)
      return;

   char line[32768];
   va_list args;
   va_start(args, format);
   int length = vsnprintf(line, sizeof(line) - 2, format, args);
   va_end(args);
   if (length < 0)
      return;
   size_t used = (size_t)length < sizeof(line) - 2 ? (size_t)length
                                                   : sizeof(line) - 2;
   line[used++] = '\n';
   line[used] = '\0';

   pthread_mutex_lock(&state_lock);
   int fd = (int)syscall(SYS_openat, AT_FDCWD, trace_path,
                         O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
   if (fd >= 0) {
      write_all(fd, line, used);
      syscall(SYS_close, fd);
   } else
      emit_error(trace_path, errno);
   pthread_mutex_unlock(&state_lock);
}

static void
initialize(void)
{
   const char *path = getenv("KBASE_ORACLE_TRACE");
   if (!path || !path[0]) {
      emit_error("KBASE_ORACLE_TRACE is not set", ENOENT);
      return;
   }

   snprintf(trace_path, sizeof(trace_path), "%s", path);
   const char *label = getenv("KBASE_ORACLE_LABEL");
   json_escape(label ? label : "unlabelled", trace_label, sizeof(trace_label));
   const char *requested = getenv("KBASE_ORACLE_SNAPSHOT_BYTES");
   if (requested && requested[0]) {
      char *end = NULL;
      unsigned long value = strtoul(requested, &end, 10);
      if (end && *end == '\0' && value >= 32 && value <= MAX_SNAPSHOT_BYTES)
         snapshot_bytes = value;
   }
   trace_enabled = true;
   emit_line("{\"schema\":1,\"type\":\"process\",\"seq\":%" PRIuFAST64
             ",\"time_ns\":%" PRIu64 ",\"pid\":%d,\"tid\":%ld,"
             "\"label\":\"%s\",\"snapshot_bytes\":%zu}",
             atomic_fetch_add(&event_sequence, 1), monotonic_ns(), getpid(),
             thread_id(), trace_label, snapshot_bytes);
}

static bool
is_kbase_fd(int fd)
{
   char proc_path[64];
   char target[512];
   snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
   ssize_t length = syscall(SYS_readlinkat, AT_FDCWD, proc_path, target,
                            sizeof(target) - 1);
   if (length <= 0)
      return false;
   target[length] = '\0';
   return strstr(target, "/dev/mali") != NULL;
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
record_mapping(uintptr_t cpu, size_t length, uint64_t offset)
{
   pthread_mutex_lock(&state_lock);
   struct allocation *allocation = find_pending_allocation(offset);
   if (!allocation) {
      pthread_mutex_unlock(&state_lock);
      return;
   }
   struct mapping *slot = NULL;
   for (size_t i = 0; i < MAX_MAPPINGS; ++i) {
      if (!mappings[i].active) {
         slot = &mappings[i];
         break;
      }
   }
   if (!slot) {
      pthread_mutex_unlock(&state_lock);
      return;
   }
   slot->cpu = cpu;
   slot->gpu = (allocation->flags & BASE_MEM_SAME_VA) ? cpu : allocation->gpu;
   slot->length = length;
   slot->allocation_id = allocation->id;
   slot->active = true;
   allocation->awaiting_mapping = false;
   if (allocation->flags & BASE_MEM_SAME_VA)
      allocation->gpu = cpu;
   uint32_t allocation_id = slot->allocation_id;
   uint64_t gpu = slot->gpu;
   pthread_mutex_unlock(&state_lock);

   emit_line("{\"schema\":1,\"type\":\"mapping\",\"seq\":%" PRIuFAST64
             ",\"time_ns\":%" PRIu64 ",\"pid\":%d,\"tid\":%ld,"
             "\"allocation\":%u,\"gpu\":\"0x%" PRIx64
             "\",\"length\":%zu}",
             atomic_fetch_add(&event_sequence, 1), monotonic_ns(), getpid(),
             thread_id(), allocation_id, gpu, length);
}

static void
forget_mapping(uintptr_t cpu, size_t length)
{
   pthread_mutex_lock(&state_lock);
   for (size_t i = 0; i < MAX_MAPPINGS; ++i) {
      uintptr_t start = mappings[i].cpu;
      uintptr_t end = start + mappings[i].length;
      uintptr_t unmap_end = cpu + length;
      if (mappings[i].active && cpu < end && unmap_end > start)
         mappings[i].active = false;
   }
   pthread_mutex_unlock(&state_lock);
}

static bool
copy_gpu(uint64_t gpu, void *destination, size_t length, uint32_t *allocation_id,
         uint64_t *offset, size_t *copied_length)
{
   bool copied = false;
   pthread_mutex_lock(&state_lock);
   for (size_t i = 0; i < MAX_MAPPINGS; ++i) {
      uint64_t start = mappings[i].gpu;
      uint64_t end = start + mappings[i].length;
      if (mappings[i].active && gpu >= start && gpu < end) {
         size_t available = (size_t)(end - gpu);
         size_t amount = length < available ? length : available;
         memcpy(destination,
                (void *)(mappings[i].cpu + (uintptr_t)(gpu - start)), amount);
         *allocation_id = mappings[i].allocation_id;
         *offset = gpu - start;
         *copied_length = amount;
         copied = true;
         break;
      }
   }
   pthread_mutex_unlock(&state_lock);
   return copied;
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
capture_job_chain(uint64_t submit_id, uint32_t atom_index, uint64_t root)
{
   uint64_t address = root;
   uint64_t visited[MAX_CHAIN_JOBS];
   size_t visited_count = 0;
   size_t bytes_to_copy = snapshot_bytes;
   uint8_t *bytes = malloc(bytes_to_copy);
   char *encoded = malloc(bytes_to_copy * 2 + 1);
   if (!bytes || !encoded) {
      free(bytes);
      free(encoded);
      return;
   }

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
      if (!copy_gpu(address, bytes, bytes_to_copy, &allocation_id, &offset,
                    &copied_length) || copied_length < 32) {
         emit_line("{\"schema\":1,\"type\":\"job\",\"seq\":%" PRIuFAST64
                   ",\"time_ns\":%" PRIu64 ",\"pid\":%d,\"tid\":%ld,"
                   "\"submit\":%" PRIu64 ",\"atom_index\":%u,"
                   "\"chain_index\":%zu,\"gpu\":\"0x%" PRIx64
                   "\",\"mapped\":false}",
                   atomic_fetch_add(&event_sequence, 1), monotonic_ns(),
                   getpid(), thread_id(), submit_id, atom_index, visited_count,
                   address);
         break;
      }

      uint32_t word4;
      uint32_t word5;
      uint64_t next;
      memcpy(&word4, bytes + 16, sizeof(word4));
      memcpy(&word5, bytes + 20, sizeof(word5));
      memcpy(&next, bytes + 24, sizeof(next));
      hex_encode(bytes, copied_length, encoded);
      emit_line("{\"schema\":1,\"type\":\"job\",\"seq\":%" PRIuFAST64
                ",\"time_ns\":%" PRIu64 ",\"pid\":%d,\"tid\":%ld,"
                "\"submit\":%" PRIu64 ",\"atom_index\":%u,"
                "\"chain_index\":%zu,\"allocation\":%u,"
                "\"offset\":%" PRIu64 ",\"type_id\":%u,\"barrier\":%s,"
                "\"index\":%u,\"dependency_1\":%u,\"dependency_2\":%u,"
                "\"next_gpu\":\"0x%" PRIx64 "\",\"bytes\":\"%s\"}",
                atomic_fetch_add(&event_sequence, 1), monotonic_ns(), getpid(),
                thread_id(), submit_id, atom_index, visited_count,
                allocation_id, offset, (word4 >> 1) & 0x7f,
                (word4 & (1u << 8)) ? "true" : "false", word4 >> 16,
                word5 & 0xffff, word5 >> 16, next, encoded);
      address = next;
      visited_count++;
   }

   free(encoded);
   free(bytes);
}

static void
capture_submit(const struct kbase_ioctl_job_submit *submit)
{
   uint64_t submit_id = atomic_fetch_add(&submit_sequence, 1);
   emit_line("{\"schema\":1,\"type\":\"submit\",\"seq\":%" PRIuFAST64
             ",\"time_ns\":%" PRIu64 ",\"pid\":%d,\"tid\":%ld,"
             "\"submit\":%" PRIu64 ",\"nr_atoms\":%u,\"stride\":%u}",
             atomic_fetch_add(&event_sequence, 1), monotonic_ns(), getpid(),
             thread_id(), submit_id, submit->nr_atoms, submit->stride);

   if (submit->nr_atoms > 4096 || submit->stride < sizeof(base_jd_atom))
      return;

   for (uint32_t i = 0; i < submit->nr_atoms; ++i) {
      base_jd_atom atom;
      memcpy(&atom, (const void *)(uintptr_t)(submit->addr +
                                               (uint64_t)i * submit->stride),
             sizeof(atom));
      emit_line("{\"schema\":1,\"type\":\"atom\",\"seq\":%" PRIuFAST64
                ",\"time_ns\":%" PRIu64 ",\"pid\":%d,\"tid\":%ld,"
                "\"submit\":%" PRIu64 ",\"atom_index\":%u,"
                "\"atom_number\":%u,\"core_req\":%u,\"jc\":\"0x%" PRIx64
                "\",\"pre_dep_0\":%u,\"pre_dep_type_0\":%u,"
                "\"pre_dep_1\":%u,\"pre_dep_type_1\":%u,\"priority\":%u,"
                "\"device\":%u,\"jobslot\":%u,\"seq_nr\":%" PRIu64 "}",
                atomic_fetch_add(&event_sequence, 1), monotonic_ns(), getpid(),
                thread_id(), submit_id, i, atom.atom_number, atom.core_req,
                atom.jc, atom.pre_dep[0].atom_id,
                atom.pre_dep[0].dependency_type, atom.pre_dep[1].atom_id,
                atom.pre_dep[1].dependency_type, atom.prio, atom.device_nr,
                atom.jobslot, atom.seq_nr);
      if (atom.jc && !(atom.core_req & BASE_JD_REQ_SOFT_JOB))
         capture_job_chain(submit_id, i, atom.jc);
   }
}

static void
capture_events(const void *buffer, size_t length)
{
   const struct base_jd_event_v2 *events = buffer;
   size_t count = length / sizeof(*events);
   for (size_t i = 0; i < count; ++i) {
      emit_line("{\"schema\":1,\"type\":\"completion\",\"seq\":%" PRIuFAST64
                ",\"time_ns\":%" PRIu64 ",\"pid\":%d,\"tid\":%ld,"
                "\"event_code\":%u,\"atom_number\":%u,"
                "\"udata_0\":\"0x%" PRIx64 "\","
                "\"udata_1\":\"0x%" PRIx64 "\"}",
                atomic_fetch_add(&event_sequence, 1), monotonic_ns(), getpid(),
                thread_id(), events[i].event_code, events[i].atom_number,
                events[i].udata.blob[0], events[i].udata.blob[1]);
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

#ifdef __ANDROID__
int
ioctl(int fd, int request_value, ...)
#else
int
ioctl(int fd, unsigned long request_value, ...)
#endif
{
   pthread_once(&init_once, initialize);
   unsigned long request = (unsigned long)request_value;
   va_list args;
   va_start(args, request_value);
   void *argument = va_arg(args, void *);
   va_end(args);

   bool kbase = trace_enabled && is_kbase_fd(fd);
   union kbase_ioctl_mem_alloc alloc_before;
   union kbase_ioctl_mem_import import_before;
   struct kbase_ioctl_mem_free free_before;
   memset(&alloc_before, 0, sizeof(alloc_before));
   memset(&import_before, 0, sizeof(import_before));
   memset(&free_before, 0, sizeof(free_before));

   if (kbase && argument && request == KBASE_IOCTL_MEM_ALLOC)
      memcpy(&alloc_before, argument, sizeof(alloc_before));
   if (kbase && argument && request == KBASE_IOCTL_MEM_IMPORT)
      memcpy(&import_before, argument, sizeof(import_before));
   if (kbase && argument && request == KBASE_IOCTL_MEM_FREE)
      memcpy(&free_before, argument, sizeof(free_before));
   if (kbase && argument && request == KBASE_IOCTL_JOB_SUBMIT)
      capture_submit(argument);

   int result = (int)syscall(SYS_ioctl, fd, request, argument);
   int saved_errno = errno;

   if (kbase) {
      emit_line("{\"schema\":1,\"type\":\"ioctl\",\"seq\":%" PRIuFAST64
                ",\"time_ns\":%" PRIu64 ",\"pid\":%d,\"tid\":%ld,"
                "\"name\":\"%s\",\"request\":\"0x%lx\",\"number\":%lu,"
                "\"direction\":%lu,\"size\":%lu,\"result\":%d,\"errno\":%d}",
                atomic_fetch_add(&event_sequence, 1), monotonic_ns(), getpid(),
                thread_id(), ioctl_name(request), request,
                (unsigned long)_IOC_NR(request),
                (unsigned long)_IOC_DIR(request),
                (unsigned long)_IOC_SIZE(request), result,
                result < 0 ? saved_errno : 0);
   }

   if (kbase && result >= 0 && argument && request == KBASE_IOCTL_MEM_ALLOC) {
      union kbase_ioctl_mem_alloc after;
      memcpy(&after, argument, sizeof(after));
      pthread_mutex_lock(&state_lock);
      struct allocation *allocation =
         record_allocation(after.out.gpu_va, after.out.gpu_va,
                           alloc_before.in.va_pages, after.out.flags);
      uint32_t id = allocation ? allocation->id : 0;
      pthread_mutex_unlock(&state_lock);
      emit_line("{\"schema\":1,\"type\":\"allocation\",\"seq\":%" PRIuFAST64
                ",\"time_ns\":%" PRIu64 ",\"pid\":%d,\"tid\":%ld,"
                "\"allocation\":%u,\"kind\":\"native\",\"pages\":%" PRIu64
                ",\"commit_pages\":%" PRIu64 ",\"flags\":\"0x%" PRIx64
                "\",\"gpu_or_cookie\":\"0x%" PRIx64 "\"}",
                atomic_fetch_add(&event_sequence, 1), monotonic_ns(), getpid(),
                thread_id(), id, alloc_before.in.va_pages,
                alloc_before.in.commit_pages, after.out.flags,
                after.out.gpu_va);
   }

   if (kbase && result >= 0 && argument && request == KBASE_IOCTL_MEM_IMPORT) {
      union kbase_ioctl_mem_import after;
      memcpy(&after, argument, sizeof(after));
      pthread_mutex_lock(&state_lock);
      struct allocation *allocation =
         record_allocation(after.out.gpu_va, after.out.gpu_va,
                           after.out.va_pages, after.out.flags);
      uint32_t id = allocation ? allocation->id : 0;
      pthread_mutex_unlock(&state_lock);
      emit_line("{\"schema\":1,\"type\":\"allocation\",\"seq\":%" PRIuFAST64
                ",\"time_ns\":%" PRIu64 ",\"pid\":%d,\"tid\":%ld,"
                "\"allocation\":%u,\"kind\":\"import\",\"pages\":%" PRIu64
                ",\"flags\":\"0x%" PRIx64 ",\"import_type\":%u,"
                "\"gpu_or_cookie\":\"0x%" PRIx64 "\"}",
                atomic_fetch_add(&event_sequence, 1), monotonic_ns(), getpid(),
                thread_id(), id, after.out.va_pages, after.out.flags,
                import_before.in.type, after.out.gpu_va);
   }

   if (kbase && result >= 0 && request == KBASE_IOCTL_MEM_FREE) {
      pthread_mutex_lock(&state_lock);
      struct allocation *allocation =
         find_gpu_allocation(free_before.gpu_addr);
      if (allocation)
         allocation->active = false;
      pthread_mutex_unlock(&state_lock);
   }

   errno = saved_errno;
   return result;
}

ssize_t
read(int fd, void *buffer, size_t count)
{
   pthread_once(&init_once, initialize);
   bool kbase = trace_enabled && is_kbase_fd(fd);
   ssize_t result = syscall(SYS_read, fd, buffer, count);
   int saved_errno = errno;
   if (kbase && result > 0)
      capture_events(buffer, (size_t)result);
   errno = saved_errno;
   return result;
}

static void *
map_common(void *address, size_t length, int protection, int flags, int fd,
           uint64_t offset)
{
   pthread_once(&init_once, initialize);
   bool kbase = trace_enabled && is_kbase_fd(fd);
   void *result = (void *)syscall(SYS_mmap, address, length, protection, flags,
                                  fd, (off_t)offset);
   int saved_errno = errno;
   if (kbase && result != MAP_FAILED)
      record_mapping((uintptr_t)result, length, offset);
   errno = saved_errno;
   return result;
}

void *
mmap(void *address, size_t length, int protection, int flags, int fd,
     off_t offset)
{
   return map_common(address, length, protection, flags, fd, (uint64_t)offset);
}

void *
mmap64(void *address, size_t length, int protection, int flags, int fd,
       off64_t offset)
{
   return map_common(address, length, protection, flags, fd, (uint64_t)offset);
}

int
munmap(void *address, size_t length)
{
   pthread_once(&init_once, initialize);
   int result = (int)syscall(SYS_munmap, address, length);
   int saved_errno = errno;
   if (trace_enabled && result == 0)
      forget_mapping((uintptr_t)address, length);
   errno = saved_errno;
   return result;
}
