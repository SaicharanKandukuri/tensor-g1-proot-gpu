/*
 * Copyright © 2021 Collabora Ltd.
 *
 * Derived from tu_device.c which is:
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "genxml/gen_macros.h"

#include "decode.h"

#include "panvk_cmd_buffer.h"
#include "panvk_device.h"
#include "panvk_entrypoints.h"
#include "panvk_event.h"
#include "panvk_image.h"
#include "panvk_image_view.h"
#include "panvk_instance.h"
#include "panvk_physical_device.h"
#include "panvk_priv_bo.h"
#include "panvk_queue.h"

#include "vk_drm_syncobj.h"
#include "vk_sync.h"
#include "vk_framebuffer.h"

#include "drm-uapi/panfrost_drm.h"
#include "drm-uapi/mali_kbase_ioctl.h"
#include <fcntl.h>
#include <inttypes.h>
#ifdef PANVK_JM_FAULT_INJECTION
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#endif
#include <poll.h>
#include <stddef.h>
#include <sys/mman.h>
#include <unistd.h>
#include "util/cnd_monotonic.h"
#include "util/libsync.h"
#include "util/os_time.h"
#include "util/timespec.h"
#include "pan_v9_trace.h"

_Static_assert(sizeof(void *) == 8, "kbase JM requires 64-bit userspace");
_Static_assert(sizeof(base_jd_atom) == 64, "base_jd_atom ABI");
_Static_assert(_Alignof(base_jd_atom) == 8, "base_jd_atom alignment");
_Static_assert(offsetof(base_jd_atom, jc) == 8, "base_jd_atom.jc ABI");
_Static_assert(offsetof(base_jd_atom, pre_dep) == 44,
               "base_jd_atom.pre_dep ABI");
_Static_assert(offsetof(base_jd_atom, atom_number) == 48,
               "base_jd_atom.atom_number ABI");
_Static_assert(offsetof(base_jd_atom, core_req) == 52,
               "base_jd_atom.core_req ABI");
_Static_assert(sizeof(struct base_jd_event_v2) == 24, "base_jd_event_v2 ABI");
_Static_assert(offsetof(struct base_jd_event_v2, event_code) == 0,
               "base_jd_event_v2.event_code ABI");
_Static_assert(offsetof(struct base_jd_event_v2, atom_number) == 4,
               "base_jd_event_v2.atom_number ABI");
_Static_assert(offsetof(struct base_jd_event_v2, udata) == 8,
               "base_jd_event_v2.udata ABI");
_Static_assert(sizeof(struct base_fence) == 8, "base_fence ABI");
_Static_assert(__builtin_offsetof(struct base_fence, basep.fd) == 0,
               "base_fence.fd ABI");
_Static_assert(__builtin_offsetof(struct base_fence, basep.stream_fd) == 4,
               "base_fence.stream_fd ABI");
_Static_assert(sizeof(struct kbase_ioctl_job_submit) == 16,
               "kbase_ioctl_job_submit ABI");
_Static_assert(offsetof(struct kbase_ioctl_job_submit, addr) == 0,
               "kbase_ioctl_job_submit.addr ABI");
_Static_assert(offsetof(struct kbase_ioctl_job_submit, nr_atoms) == 8,
               "kbase_ioctl_job_submit.nr_atoms ABI");
_Static_assert(offsetof(struct kbase_ioctl_job_submit, stride) == 12,
               "kbase_ioctl_job_submit.stride ABI");
_Static_assert(_IOC_TYPE(KBASE_IOCTL_JOB_SUBMIT) == KBASE_IOCTL_TYPE,
               "KBASE_IOCTL_JOB_SUBMIT type ABI");
_Static_assert(_IOC_NR(KBASE_IOCTL_JOB_SUBMIT) == 2,
               "KBASE_IOCTL_JOB_SUBMIT number ABI");
_Static_assert(_IOC_DIR(KBASE_IOCTL_JOB_SUBMIT) == _IOC_WRITE,
               "KBASE_IOCTL_JOB_SUBMIT direction ABI");
_Static_assert(_IOC_SIZE(KBASE_IOCTL_JOB_SUBMIT) == 16,
               "KBASE_IOCTL_JOB_SUBMIT size ABI");
_Static_assert(BASE_JD_DEP_TYPE_DATA == 1, "JM data dependency ABI");
_Static_assert(BASE_JD_DEP_TYPE_ORDER == 2, "JM order dependency ABI");
_Static_assert(BASE_JD_REQ_DEP == 0, "JM dependency atom ABI");
_Static_assert(BASE_JD_REQ_SOFT_JOB == 512, "JM soft job ABI");
_Static_assert(BASE_JD_REQ_SOFT_FENCE_TRIGGER == 514,
               "JM fence trigger ABI");
_Static_assert(BASE_JD_REQ_SOFT_FENCE_WAIT == 515, "JM fence wait ABI");
_Static_assert(BASE_JD_REQ_EVENT_ONLY_ON_FAILURE == 4096,
               "JM event suppression ABI");
_Static_assert(BASE_JD_EVENT_DONE == 1, "JM done event ABI");

/*
 * Dumps the first bytes of the tiler heap so a caller can diff the contents
 * before/after a submit and tell whether the tiler wrote anything at all.
 *
 * The heap is allocated with PAN_KMOD_BO_FLAG_ALLOC_ON_FAULT, which makes
 * kbase pass commit_pages=0 (see kbase_kmod.c), so the mapping exists but has
 * no pages behind it until the GPU faults them in. Dereferencing addr.host
 * directly therefore SIGSEGVs whenever the tiler did *not* write - which is
 * exactly the case we are trying to observe. Reading through /proc/self/mem
 * instead turns that into an EIO/EFAULT return value we can report.
 *
 * mincore() is only used as a hint: on some kernels it does not report
 * residency for device mappings, so a "not resident" answer there is not
 * treated as conclusive - the pread is what decides.
 */
/*
 * Where the heap dump and submit results go.
 *
 * stderr is not usable under a Winlator/Ludashi-style container: nothing
 * collects it, so the output is simply lost. Set PANVK_HEAP_LOG to a writable
 * path to capture it instead, e.g.
 *
 *    PANVK_HEAP_LOG=/sdcard/Download/panvk-heap.log
 *
 * Unset (or unopenable) means fall back to stderr, which keeps the behaviour
 * of the original patch. The file is opened once, appended to, and line
 * buffered so a crash mid-run still leaves the lines already written.
 */
static FILE *
panvk_debug_log_file(void)
{
   static FILE *log_file = NULL;
   static bool tried = false;

   if (!tried) {
      tried = true;
      const char *path = getenv("PANVK_HEAP_LOG");

      if (path && path[0]) {
         log_file = fopen(path, "ae");
         if (log_file) {
            setvbuf(log_file, NULL, _IOLBF, 0);
            fprintf(log_file, "[PANVK_DEBUG_HEAP] --- log dibuka, pid=%d ---\n",
                    (int)getpid());
         } else {
            fprintf(stderr,
                    "[PANVK_DEBUG_HEAP] gagal membuka PANVK_HEAP_LOG=%s: %s,"
                    " jatuh ke stderr\n",
                    path, strerror(errno));
         }
      }
   }

   return log_file ? log_file : stderr;
}

static void
panvk_debug_dump_tiler_heap(struct panvk_device *dev, const char *label)
{
   FILE *out = panvk_debug_log_file();

   if (!dev->tiler_heap || !dev->tiler_heap->addr.host) {
      fprintf(out, "[PANVK_DEBUG_HEAP] %s: tiler_heap belum ada/belum ke-mmap\n",
              label);
      return;
   }

   void *host = dev->tiler_heap->addr.host;
   const size_t dump_len = 64;

   fprintf(out,
           "[PANVK_DEBUG_HEAP] %s bo->flags=0x%x dev=0x%" PRIx64
           " size=%zu host=%p",
           label, dev->tiler_heap->bo->flags, dev->tiler_heap->addr.dev,
           (size_t)pan_kmod_bo_size(dev->tiler_heap->bo), host);

   unsigned char resident = 0;
   long page_size = sysconf(_SC_PAGESIZE);
   if (page_size > 0 &&
       mincore((void *)((uintptr_t)host & ~(uintptr_t)(page_size - 1)),
               page_size, &resident) == 0) {
      fprintf(out, " mincore=%s", (resident & 1) ? "resident" : "absent");
   } else {
      fprintf(out, " mincore=unavailable");
   }

   /* Safe read: never dereference host directly, see comment above. */
   uint8_t buf[64];
   int mem_fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
   if (mem_fd < 0) {
      fprintf(out, " -- open(/proc/self/mem) gagal: %s\n", strerror(errno));
      return;
   }

   ssize_t n = pread(mem_fd, buf, dump_len, (off_t)(uintptr_t)host);
   int saved_errno = errno;
   close(mem_fd);

   if (n < 0) {
      /* EIO here means the page is mapped but not backed: the GPU never
       * faulted it in, i.e. nothing wrote to the heap. */
      fprintf(out, " -- UNREADABLE (%s) => halaman belum ter-commit,"
                      " tiler tidak menulis apa pun\n",
              strerror(saved_errno));
      return;
   }

   bool all_zero = true;
   for (ssize_t i = 0; i < n; i++) {
      if (buf[i] != 0) {
         all_zero = false;
         break;
      }
   }

   fprintf(out, " first%zd%s:", n, all_zero ? " (ALL ZERO)" : "");
   for (ssize_t i = 0; i < n; i++)
      fprintf(out, " %02x", buf[i]);
   fprintf(out, "\n");
}

/*
 * Reads back every job header in the batch AFTER the GPU ran it.
 *
 * Job Header (v9.xml) starts with the fields the GPU itself writes:
 *
 *   Exception Status       word 0
 *   First Incomplete Task  word 1
 *   Fault Pointer          word 2..3
 *
 * The pre-submit dump always shows these as zero because nothing has run yet,
 * so a job that is silently rejected by the GPU looks identical to one that
 * succeeded. Reading them back after the wait is the only way to tell apart
 * "the GPU ran this and wrote nothing" from "the GPU refused this".
 *
 * Exception Status is a BASE_JD_EVENT_* code. Verified against the r54p1 UAPI
 * header (jm/mali_base_jm_kernel.h):
 *
 *   BASE_JD_EVENT_NOT_STARTED = 0x00
 *   BASE_JD_EVENT_DONE        = 0x01   <- success
 *   BASE_JD_EVENT_STOPPED     = 0x03
 *   BASE_JD_EVENT_TERMINATED  = 0x04
 *   BASE_JD_EVENT_ACTIVE      = 0x08
 *   ...RANGE_HW_FAULT_OR_SW_ERROR_START = 0x40
 *
 * So 0x01 is *success*, and anything below 0x40 is non-fatal. Only >= 0x40 is
 * an actual fault. Do not treat "non-zero" as an error here - an earlier
 * version of this print did and sent the investigation down a dead end.
 *
 * batch->jobs holds the CPU pointers of every job descriptor in the batch,
 * which is exactly what we need - these are pool allocations, still mapped.
 */
#define PANVK_JD_EVENT_NOT_STARTED 0x00
#define PANVK_JD_EVENT_DONE        0x01
#define PANVK_JD_EVENT_FAULT_START 0x40

static const char *
panvk_jd_event_str(uint32_t code)
{
   switch (code) {
   case PANVK_JD_EVENT_NOT_STARTED:
      return "NOT_STARTED";
   case PANVK_JD_EVENT_DONE:
      return "DONE";
   case 0x03:
      return "STOPPED";
   case 0x04:
      return "TERMINATED";
   case 0x08:
      return "ACTIVE";
   default:
      return code >= PANVK_JD_EVENT_FAULT_START ? "FAULT" : "non-fatal";
   }
}

static void
panvk_debug_dump_job_status(struct panvk_cmd_buffer *cmdbuf,
                            struct panvk_batch *batch, const char *label)
{
   FILE *out = panvk_debug_log_file();
   unsigned idx = 0;

   /* The descriptor pool is CPU-cached, so invalidate before reading back
    * anything the GPU may have written. */
   panvk_pool_invalidate_maps(&cmdbuf->desc_pool);

   util_dynarray_foreach(&batch->jobs, void *, job) {
      const uint8_t *bytes = *job;
      uint32_t exception_status, first_incomplete_task, word4, word5;
      uint64_t fault_pointer, next_job;

      memcpy(&exception_status, bytes + 0, sizeof(exception_status));
      memcpy(&first_incomplete_task, bytes + 4, sizeof(first_incomplete_task));
      memcpy(&fault_pointer, bytes + 8, sizeof(fault_pointer));
      memcpy(&word4, bytes + 16, sizeof(word4));
      /* Job Header, v9.xml: Index is word 4 bits 16..31, Dependency 1 and 2 are
       * word 5 low and high halves, Next is the 64-bit word 6. Reading these
       * back tells us the dependency graph the hardware was actually given,
       * which the job type alone does not. */
      memcpy(&word5, bytes + 20, sizeof(word5));
      memcpy(&next_job, bytes + 24, sizeof(next_job));

      fprintf(out,
              "[PANVK_DEBUG_JOBSTATUS] %s job[%u] type=%u"
              " index=%u dep1=%u dep2=%u next=0x%" PRIx64
              " exception_status=0x%02x (%s) first_incomplete_task=%u"
              " fault_pointer=0x%" PRIx64 "%s\n",
              label, idx, (word4 >> 1) & BITFIELD_MASK(7),
              (word4 >> 16) & 0xffff, word5 & 0xffff, (word5 >> 16) & 0xffff,
              next_job, exception_status,
              panvk_jd_event_str(exception_status), first_incomplete_task,
              fault_pointer,
              exception_status >= PANVK_JD_EVENT_FAULT_START
                 ? "  <== GPU FAULT"
                 : "");
      idx++;
   }
}

/* ---- kbase JM submission: kernel-side chaining (8.9), async completion (8.15)
 *
 * 8.9: the atoms of a vkQueueSubmit are queued with a DATA dependency on the
 * previous atom -- the order of the old submit-and-wait loop, enforced by
 * kbase -- and go to the kernel in one JOB_SUBMIT. Only the last atom reports
 * success; a failed atom reports itself and cancels its dependents.
 *
 * 8.15: vkQueueSubmit no longer waits for its atoms. Each JOB_SUBMIT is a
 * submission with a seqno; whichever thread needs progress (a CPU wait, a
 * status query, an allocation short of atom numbers) reads the kbase
 * completion events, one reader at a time, and advances completed_seqno (a
 * dedicated event thread cost ~0.1-0.25 ms per wait in two wake-ups,
 * measured). A fence or semaphore signalled by a
 * submit is pending on its seqno: a CPU wait blocks on completed_seqno, and a
 * SYNC_FD export submits a FENCE_TRIGGER soft atom that depends on the
 * submission's last atom -- a real sync_file, signalled by kbase. A waited-on
 * imported sync_file becomes a FENCE_WAIT soft atom that the first job atom
 * depends on, so the GPU waits for it instead of the CPU. Every submission's
 * first atom depends (ORDER) on the previous submission's last atom: work on
 * the queue still runs in submission order, a wait on our own earlier work
 * needs nothing, and the shared tiler heap stays serialised.
 *
 * Kernel behaviour relied on (a16w_jm r54p1 source, and fenceprobe on the
 * RS4): an atom number can be reused once its event has been read, or at once
 * if the event was suppressed on success; a dependency on a completed atom is
 * met if that atom ended DONE; an ORDER dependency does not pass a failure on;
 * the FENCE_TRIGGER fd is written back during the JOB_SUBMIT ioctl.
 *
 * PANVK_KBASE_WAIT_SUBMIT=1 restores the 8.14 behaviour: waits on the CPU
 * before, the submit waits for its atoms, signals on the CPU, exports as a CPU
 * wait + -1. PANVK_KBASE_CPU_FENCE_WAIT=1 waits for imported sync_files on
 * the CPU; PANVK_KBASE_CPU_FENCE_EXPORT=1 exports as a CPU wait + -1.
 * PANVK_KBASE_SYNC_SUBMIT=1 (8.9's per-chain control) implies WAIT_SUBMIT.
 * Negative controls, broken on purpose: PANVK_KBASE_CTRL_NO_FENCE_WAIT_DEP=1
 * (work does not wait for imported sync_files), PANVK_KBASE_CTRL_EXPORT_EARLY=1
 * (an export claims the work is done: -1, without waiting). */
#define PANVK_KBASE_MAX_CHAIN 120
#define PANVK_KBASE_ATOM_STRIDE sizeof(base_jd_atom)
/* The job atoms of one submission, plus a FENCE_WAIT and a DEP atom. */
#define PANVK_KBASE_MAX_SUB_ATOMS (PANVK_KBASE_MAX_CHAIN + 2)
/* Each submission in flight holds at least one of the 255 atom numbers. */
#define PANVK_KBASE_MAX_SUBS 255

struct panvk_kbase_job {
   uint64_t jc;
   uint32_t core_req;
};

/* The job atoms of the vkQueueSubmit being built, not yet in the kernel. */
struct panvk_kbase_chain {
   struct panvk_kbase_job jobs[PANVK_KBASE_MAX_CHAIN];
   struct panvk_batch *batches[PANVK_KBASE_MAX_CHAIN];
   unsigned nr_jobs, nr_batches;
   /* Merged imported sync_files the next submission waits for, or -1. */
   int wait_fd;
};

struct panvk_kbase_sub {
   uint64_t seqno;
   uint8_t last; /* the atom whose event completes the submission */
   bool done;
   int fence_fd; /* out-fence made by an export, -1 until then */
   uint16_t nr_atoms;
   uint8_t atoms[PANVK_KBASE_MAX_SUB_ATOMS];
};

enum panvk_kbase_atom_use {
   PANVK_KBASE_ATOM_FREE = 0,
   PANVK_KBASE_ATOM_SUB, /* in a submission, freed when it completes */
   PANVK_KBASE_ATOM_AUX, /* an export's FENCE_TRIGGER, freed by its event */
};

#ifdef PANVK_JM_FAULT_INJECTION
enum panvk_jm_fault_mode {
   PANVK_JM_FAULT_NONE,
   PANVK_JM_FAULT_SUBMIT_EIO,
   PANVK_JM_FAULT_PPOLL_EIO,
   PANVK_JM_FAULT_PPOLL_TIMEOUT,
   PANVK_JM_FAULT_READ_EIO,
   PANVK_JM_FAULT_DONE_FAILURE,
};

struct panvk_jm_fault {
   enum panvk_jm_fault_mode mode;
   char run_id[64];
   unsigned target;
   atomic_uint remaining;
   atomic_bool consumed;
};

static const char *
panvk_jm_fault_mode_name(enum panvk_jm_fault_mode mode)
{
   switch (mode) {
   case PANVK_JM_FAULT_SUBMIT_EIO:
      return "submit-eio";
   case PANVK_JM_FAULT_PPOLL_EIO:
      return "ppoll-eio";
   case PANVK_JM_FAULT_PPOLL_TIMEOUT:
      return "ppoll-timeout";
   case PANVK_JM_FAULT_READ_EIO:
      return "read-eio";
   case PANVK_JM_FAULT_DONE_FAILURE:
      return "done-failure";
   default:
      return "none";
   }
}

static enum panvk_jm_fault_mode
panvk_jm_fault_parse_mode(const char *name, size_t len)
{
   static const struct {
      const char *name;
      enum panvk_jm_fault_mode mode;
   } modes[] = {
      {"submit-eio", PANVK_JM_FAULT_SUBMIT_EIO},
      {"ppoll-eio", PANVK_JM_FAULT_PPOLL_EIO},
      {"ppoll-timeout", PANVK_JM_FAULT_PPOLL_TIMEOUT},
      {"read-eio", PANVK_JM_FAULT_READ_EIO},
      {"done-failure", PANVK_JM_FAULT_DONE_FAILURE},
   };

   for (unsigned i = 0; i < ARRAY_SIZE(modes); i++) {
      if (strlen(modes[i].name) == len && !memcmp(name, modes[i].name, len))
         return modes[i].mode;
   }
   return PANVK_JM_FAULT_NONE;
}

static bool
panvk_jm_fault_parse(const char *selector, struct panvk_jm_fault *fault)
{
   const char *mode_sep = strchr(selector, ':');
   if (!mode_sep || mode_sep == selector)
      return false;

   const size_t run_len = mode_sep - selector;
   if (run_len >= sizeof(fault->run_id))
      return false;
   for (size_t i = 0; i < run_len; i++) {
      const char c = selector[i];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
         return false;
   }

   const char *count_sep = strchr(mode_sep + 1, ':');
   const size_t mode_len =
      count_sep ? (size_t)(count_sep - mode_sep - 1) : strlen(mode_sep + 1);
   enum panvk_jm_fault_mode mode =
      panvk_jm_fault_parse_mode(mode_sep + 1, mode_len);
   if (mode == PANVK_JM_FAULT_NONE)
      return false;

   unsigned long target = 1;
   if (count_sep) {
      if (!count_sep[1])
         return false;
      for (const char *c = count_sep + 1; *c; c++) {
         if (*c < '0' || *c > '9')
            return false;
      }
      char *end = NULL;
      target = strtoul(count_sep + 1, &end, 10);
      if (end == count_sep + 1 || *end || !target || target > UINT_MAX)
         return false;
   }

   memcpy(fault->run_id, selector, run_len);
   fault->run_id[run_len] = '\0';
   fault->mode = mode;
   fault->target = target;
   atomic_init(&fault->remaining, target);
   atomic_init(&fault->consumed, false);
   return true;
}

static bool
panvk_jm_fault_parser_self_test(void)
{
   struct panvk_jm_fault fault = {0};
   return panvk_jm_fault_parse("self:submit-eio:3", &fault) &&
          fault.mode == PANVK_JM_FAULT_SUBMIT_EIO && fault.target == 3 &&
          !strcmp(fault.run_id, "self") &&
          !panvk_jm_fault_parse("self:unknown", &fault) &&
          !panvk_jm_fault_parse("self:read-eio:0", &fault) &&
          !panvk_jm_fault_parse("bad\nrun:read-eio", &fault) &&
          !panvk_jm_fault_parse("bad run:read-eio", &fault) &&
          !panvk_jm_fault_parse("self:read-eio:+1", &fault) &&
          !panvk_jm_fault_parse("self:read-eio:-1", &fault);
}
#endif

struct panvk_kbase_jm {
   int fd;
   /* Protects everything below, and is held across JOB_SUBMIT so that seqno
    * order is kernel submission order. The reader polls and reads without
    * it. */
   mtx_t mtx;
   struct u_cnd_monotonic cnd; /* completions processed, or reader done */
   uint64_t last_seqno;
   uint64_t completed_seqno;
   struct panvk_kbase_sub subs[PANVK_KBASE_MAX_SUBS]; /* ring, oldest first */
   unsigned head, count;
   uint8_t atom_use[256];
   bool atom_seen[256];
   unsigned nr_free, cursor;
   bool lost;
   bool reading; /* a thread is polling/reading the kbase fd */
   /* Only touched by the submitting thread (queue access is synchronised). */
   struct panvk_kbase_chain chain;
   bool wait_submit, cpu_fence_wait, cpu_fence_export;
   bool ctrl_no_fence_wait_dep, ctrl_export_early;
   bool trace_limit_pending;
   /* Counted always, printed at destroy with PANVK_KBASE_FENCE_TRACE=1:
    * waits by payload, FENCE_WAIT atoms, exports by outcome. */
   bool trace;
#ifdef PANVK_JM_FAULT_INJECTION
   struct panvk_jm_fault fault;
#endif
   struct {
      uint64_t wait_none, wait_own, wait_file, wait_cpu, fence_wait_atoms;
      uint64_t export_new, export_dup, export_done, export_cpu;
      uint64_t atom_allocated, atom_freed, atom_reused, cursor_wraps;
      uint64_t alloc_pressure_waits, alloc_pressure_iterations, max_live_atoms;
      uint64_t events, failed_events, retired_submissions;
      uint64_t chain_flushes, chain_jobs, chain_batches;
      uint64_t limit_flushes, limit_jobs, limit_batches;
      uint64_t limit_tail_flushes, limit_tail_jobs, limit_tail_batches;
   } stats;
};

#ifdef PANVK_JM_FAULT_INJECTION
static bool
panvk_jm_fault_try_consume(struct panvk_kbase_jm *k,
                           enum panvk_jm_fault_mode mode)
{
   if (k->fault.mode != mode)
      return false;

   unsigned remaining =
      atomic_load_explicit(&k->fault.remaining, memory_order_relaxed);
   while (remaining) {
      if (atomic_compare_exchange_weak_explicit(
             &k->fault.remaining, &remaining, remaining - 1,
             memory_order_relaxed, memory_order_relaxed)) {
         if (remaining != 1)
            return false;
         atomic_store_explicit(&k->fault.consumed, true, memory_order_release);
         return true;
      }
   }
   return false;
}

static void
panvk_jm_fault_log(struct panvk_kbase_jm *k, const char *state, int error)
{
   mesa_logi("panvk: jm-fault run=%s mode=%s state=%s target=%u remaining=%u"
             " last_seqno=%" PRIu64 " completed_seqno=%" PRIu64
             " ring_head=%u ring_count=%u free_atoms=%u lost=%u"
             " failed_events_observed_injected=%" PRIu64 " error=%d",
             k->fault.run_id, panvk_jm_fault_mode_name(k->fault.mode), state,
             k->fault.target,
             atomic_load_explicit(&k->fault.remaining, memory_order_relaxed),
             k->last_seqno, k->completed_seqno, k->head, k->count, k->nr_free,
             k->lost, k->stats.failed_events, error);
}
#endif

static bool
panvk_kbase_chain_enabled(void)
{
   /* The trace/dump paths decode each chain right after it ran. */
   return !getenv("PANVK_KBASE_SYNC_SUBMIT") && !pan_v9_trace_enabled() &&
          !getenv("PANVK_HEAP_LOG") && !PANVK_DEBUG(TRACE) && !PANVK_DEBUG(DUMP);
}

static void
panvk_kbase_atom(base_jd_atom *a, uint8_t num, uint64_t jc, uint32_t core_req,
                 uint8_t dep0, uint8_t type0, uint8_t dep1, uint8_t type1)
{
   *a = (base_jd_atom){
      .jc = jc,
      .pre_dep = {
         {.atom_id = dep0, .dependency_type = dep0 ? type0 : 0},
         {.atom_id = dep1, .dependency_type = dep1 ? type1 : 0},
      },
      .atom_number = num,
      .core_req = core_req,
   };
}

static void
panvk_kbase_free_atoms(struct panvk_kbase_jm *k, unsigned n,
                       const uint8_t *nums)
{
   for (unsigned i = 0; i < n; i++) {
      assert(k->atom_use[nums[i]] != PANVK_KBASE_ATOM_FREE);
      k->atom_use[nums[i]] = PANVK_KBASE_ATOM_FREE;
   }
   k->nr_free += n;
   k->stats.atom_freed += n;
}

static struct panvk_kbase_sub *
panvk_kbase_find_sub(struct panvk_kbase_jm *k, uint8_t atom)
{
   for (unsigned i = 0; i < k->count; i++) {
      struct panvk_kbase_sub *sub =
         &k->subs[(k->head + i) % PANVK_KBASE_MAX_SUBS];
      for (unsigned j = 0; j < sub->nr_atoms; j++) {
         if (sub->atoms[j] == atom)
            return sub;
      }
   }
   return NULL;
}

/* Caller holds k->mtx. */
static void
panvk_kbase_handle_event(struct panvk_kbase_jm *k,
                         const struct base_jd_event_v2 *ev)
{
   const uint8_t atom = ev->atom_number;
   uint32_t event_code = ev->event_code;

#ifdef PANVK_JM_FAULT_INJECTION
   bool injected_failure = false;
   if (event_code == BASE_JD_EVENT_DONE &&
       k->atom_use[atom] == PANVK_KBASE_ATOM_SUB &&
       panvk_kbase_find_sub(k, atom) != NULL &&
       panvk_jm_fault_try_consume(k, PANVK_JM_FAULT_DONE_FAILURE)) {
      event_code = PANVK_JD_EVENT_FAULT_START;
      injected_failure = true;
   }
#endif

   k->stats.events++;
   if (event_code != BASE_JD_EVENT_DONE)
      k->stats.failed_events++;

   if (k->atom_use[atom] == PANVK_KBASE_ATOM_AUX) {
      if (event_code != BASE_JD_EVENT_DONE)
         mesa_loge("panvk: kbase out-fence atom %u: event 0x%x", atom,
                   event_code);
      panvk_kbase_free_atoms(k, 1, &atom);
      return;
   }

   struct panvk_kbase_sub *sub = panvk_kbase_find_sub(k, atom);
   if (!sub) {
      mesa_loge("panvk: kbase event 0x%x for unknown atom %u", event_code,
                atom);
      k->lost = true;
      return;
   }

   if (event_code != BASE_JD_EVENT_DONE) {
#ifdef PANVK_JM_FAULT_INJECTION
      if (!injected_failure)
#endif
         mesa_loge("panvk: kbase atom %u of submission %" PRIu64
                   " failed: event 0x%x",
                   atom, sub->seqno, event_code);
      k->lost = true;
#ifdef PANVK_JM_FAULT_INJECTION
      if (injected_failure)
         panvk_jm_fault_log(k, "CONSUMED", 0);
#endif
      return;
   }
   if (atom == sub->last)
      sub->done = true;

   /* Submissions complete in order: retire the finished ones. */
   while (k->count && k->subs[k->head].done) {
      struct panvk_kbase_sub *s = &k->subs[k->head];
      panvk_kbase_free_atoms(k, s->nr_atoms, s->atoms);
      if (s->fence_fd >= 0)
         close(s->fence_fd);
      k->completed_seqno = s->seqno;
      k->head = (k->head + 1) % PANVK_KBASE_MAX_SUBS;
      k->count--;
      k->stats.retired_submissions++;
   }
}

/* Make progress on completions: wait (until abs_timeout_ns) for kbase events,
 * read and process them. One thread reads at a time, the others wait for its
 * broadcast. Caller holds k->mtx, which is dropped while polling. Returns
 * false if nothing happened before the timeout. */
static bool
panvk_kbase_progress_locked(struct panvk_kbase_jm *k, uint64_t abs_timeout_ns)
{
   if (k->reading) {
      if (!abs_timeout_ns)
         return false;
      struct timespec ts;
      timespec_from_nsec(&ts, abs_timeout_ns);
      return u_cnd_monotonic_timedwait(&k->cnd, &k->mtx, &ts) != thrd_timedout;
   }

#ifdef PANVK_JM_FAULT_INJECTION
   const bool fault_has_pending = k->count > 0;
   const bool injected_timeout =
      fault_has_pending && abs_timeout_ns != 0 &&
      abs_timeout_ns != OS_TIMEOUT_INFINITE &&
      panvk_jm_fault_try_consume(k, PANVK_JM_FAULT_PPOLL_TIMEOUT);
   const bool injected_ppoll_eio =
      fault_has_pending &&
      panvk_jm_fault_try_consume(k, PANVK_JM_FAULT_PPOLL_EIO);
#endif
   k->reading = true;
   mtx_unlock(&k->mtx);

   struct pollfd pfd = {.fd = k->fd, .events = POLLIN};
   int ret;
#ifdef PANVK_JM_FAULT_INJECTION
   bool injected_failure = injected_ppoll_eio;
#endif
   do {
      struct timespec rel, *timeout = NULL;
      if (abs_timeout_ns != OS_TIMEOUT_INFINITE) {
         uint64_t now = os_time_get_nano();
         timespec_from_nsec(&rel, abs_timeout_ns > now ? abs_timeout_ns - now : 0);
         timeout = &rel;
      }
#ifdef PANVK_JM_FAULT_INJECTION
      if (injected_timeout)
         ret = ppoll(NULL, 0, timeout, NULL);
      else if (injected_ppoll_eio) {
         errno = EIO;
         ret = -1;
      } else
#endif
         ret = ppoll(&pfd, 1, timeout, NULL);
   } while (ret < 0 && errno == EINTR);
#ifdef PANVK_JM_FAULT_INJECTION
   const int injected_timeout_error =
      injected_timeout && ret != 0 ? (ret < 0 ? errno : EPROTO) : 0;
#endif

   /* poll() said there are events and nobody else reads: read() returns
    * every pending event that fits without blocking. */
   struct base_jd_event_v2 ev[32];
   ssize_t len = 0;
   bool failed = ret < 0;
   if (ret > 0 && (pfd.revents & POLLIN)) {
#ifdef PANVK_JM_FAULT_INJECTION
      if (fault_has_pending &&
          panvk_jm_fault_try_consume(k, PANVK_JM_FAULT_READ_EIO)) {
         injected_failure = true;
         errno = EIO;
         len = -1;
      } else
#endif
         len = read(k->fd, ev, sizeof(ev));
      failed = len <= 0;
      if (len > 0 && len % sizeof(ev[0])) {
         errno = EPROTO;
         len = 0;
         failed = true;
      }
   } else if (ret > 0) {
      failed = true; /* POLLERR / POLLHUP / POLLNVAL */
   }
#ifdef PANVK_JM_FAULT_INJECTION
   if (failed && !injected_failure && !injected_timeout_error)
#else
   if (failed)
#endif
      mesa_loge("panvk: kbase event poll/read failed: %s", strerror(errno));

   mtx_lock(&k->mtx);
#ifdef PANVK_JM_FAULT_INJECTION
   if (injected_timeout && ret == 0)
      panvk_jm_fault_log(k, "CONSUMED", 0);
#endif
   for (unsigned i = 0; len > 0 && !k->lost && i < len / sizeof(ev[0]); i++)
      panvk_kbase_handle_event(k, &ev[i]);
   if (failed)
      k->lost = true;
#ifdef PANVK_JM_FAULT_INJECTION
   if (injected_timeout_error)
      panvk_jm_fault_log(k, "INJECTION_ERROR", injected_timeout_error);
   else if (injected_failure)
      panvk_jm_fault_log(k, "CONSUMED", 0);
#endif
   k->reading = false;
   u_cnd_monotonic_broadcast(&k->cnd);
   return ret != 0;
}

/* Caller holds k->mtx. May wait (dropping it) for completions to free
 * numbers. */
static VkResult
panvk_kbase_alloc_atoms(struct panvk_kbase_jm *k, unsigned n, uint8_t *nums,
                        enum panvk_kbase_atom_use use)
{
   assert(n > 0 && n < 256);
   if (k->lost)
      return VK_ERROR_DEVICE_LOST;
   if (k->nr_free < n)
      k->stats.alloc_pressure_waits++;
   while (k->nr_free < n) {
      if (k->lost)
         return VK_ERROR_DEVICE_LOST;
      k->stats.alloc_pressure_iterations++;
      panvk_kbase_progress_locked(k, OS_TIMEOUT_INFINITE);
   }
   if (k->lost)
      return VK_ERROR_DEVICE_LOST;

   for (unsigned i = 0; i < n; i++) {
      while (k->atom_use[k->cursor] != PANVK_KBASE_ATOM_FREE) {
         if (k->cursor++ == 255) {
            k->cursor = 1;
            k->stats.cursor_wraps++;
         }
      }
      nums[i] = k->cursor;
      k->atom_use[k->cursor] = use;
      if (k->atom_seen[k->cursor])
         k->stats.atom_reused++;
      k->atom_seen[k->cursor] = true;
      if (k->cursor++ == 255) {
         k->cursor = 1;
         k->stats.cursor_wraps++;
      }
   }
   k->nr_free -= n;
   k->stats.atom_allocated += n;
   k->stats.max_live_atoms = MAX2(k->stats.max_live_atoms, 255 - k->nr_free);
   return VK_SUCCESS;
}

/* Submit n job atoms (and/or a wait for wait_fd) as one submission. Caller
 * holds k->mtx. *seqno is the new submission's, or the newest one's when
 * there is nothing to submit. */
static VkResult
panvk_kbase_submit_locked(struct panvk_kbase_jm *k,
                          const struct panvk_kbase_job *jobs, unsigned n,
                          int wait_fd, uint64_t *seqno)
{
   if (k->lost)
      return VK_ERROR_DEVICE_LOST;

   if (!n && wait_fd < 0) {
      *seqno = k->last_seqno;
      return VK_SUCCESS;
   }

   const unsigned nr_atoms = n + (wait_fd >= 0) + (n == 0);
   uint8_t nums[PANVK_KBASE_MAX_SUB_ATOMS];
   VkResult result =
      panvk_kbase_alloc_atoms(k, nr_atoms, nums, PANVK_KBASE_ATOM_SUB);
   if (result != VK_SUCCESS)
      return result;

   /* The previous submission's last atom, while it is still in flight. */
   const uint8_t prev =
      k->count
         ? k->subs[(k->head + k->count - 1) % PANVK_KBASE_MAX_SUBS].last
         : 0;
   base_jd_atom atoms[PANVK_KBASE_MAX_SUB_ATOMS];
   struct base_fence wait_fence = {
      .basep = {.fd = wait_fd, .stream_fd = -1},
   };
   uint8_t wait_atom = 0;
   unsigned a = 0;

   if (wait_fd >= 0) {
      k->stats.fence_wait_atoms++;
      wait_atom = nums[a];
      panvk_kbase_atom(&atoms[a], nums[a], (uintptr_t)&wait_fence,
                       BASE_JD_REQ_SOFT_FENCE_WAIT |
                          BASE_JD_REQ_EVENT_ONLY_ON_FAILURE,
                       0, 0, 0, 0);
      a++;
   }

   /* ORDER on the previous submission and on the wait: a failure there does
    * not cancel this work (an errored fence was waited for like a signalled
    * one before 8.15, too). */
   for (unsigned i = 0; i < n; i++, a++) {
      uint32_t core_req = jobs[i].core_req;
      if (i + 1 < n)
         core_req |= BASE_JD_REQ_EVENT_ONLY_ON_FAILURE;
      if (i == 0)
         panvk_kbase_atom(&atoms[a], nums[a], jobs[i].jc, core_req, prev,
                          BASE_JD_DEP_TYPE_ORDER,
                          k->ctrl_no_fence_wait_dep ? 0 : wait_atom,
                          BASE_JD_DEP_TYPE_ORDER);
      else
         panvk_kbase_atom(&atoms[a], nums[a], jobs[i].jc, core_req,
                          nums[a - 1],
                          BASE_JD_DEP_TYPE_DATA, 0, 0);
   }

   if (!n) {
      /* Only a wait: a dependency-only atom completes the submission. */
      panvk_kbase_atom(&atoms[a], nums[a], 0, BASE_JD_REQ_DEP, prev,
                       BASE_JD_DEP_TYPE_ORDER, wait_atom,
                       BASE_JD_DEP_TYPE_ORDER);
      a++;
   }
   assert(a == nr_atoms);

   struct kbase_ioctl_job_submit submit = {
      .addr = (uint64_t)(uintptr_t)atoms,
      .nr_atoms = nr_atoms,
      .stride = PANVK_KBASE_ATOM_STRIDE,
   };
#ifdef PANVK_JM_FAULT_INJECTION
   bool injected_submit =
      panvk_jm_fault_try_consume(k, PANVK_JM_FAULT_SUBMIT_EIO);
   if (injected_submit)
      errno = EIO;
   if (injected_submit || ioctl(k->fd, KBASE_IOCTL_JOB_SUBMIT, &submit)) {
#else
   if (ioctl(k->fd, KBASE_IOCTL_JOB_SUBMIT, &submit)) {
#endif
      /* Some atoms may be in the kernel: keep their numbers, give up. */
#ifdef PANVK_JM_FAULT_INJECTION
      if (!injected_submit)
#endif
         mesa_loge("panvk: kbase JOB_SUBMIT of %u atoms failed: %s", nr_atoms,
                   strerror(errno));
      k->lost = true;
#ifdef PANVK_JM_FAULT_INJECTION
      if (injected_submit)
         panvk_jm_fault_log(k, "CONSUMED", 0);
#endif
      u_cnd_monotonic_broadcast(&k->cnd);
      return VK_ERROR_DEVICE_LOST;
   }

   assert(k->count < PANVK_KBASE_MAX_SUBS);
   struct panvk_kbase_sub *sub =
      &k->subs[(k->head + k->count) % PANVK_KBASE_MAX_SUBS];
   k->count++;
   *sub = (struct panvk_kbase_sub){
      .seqno = ++k->last_seqno,
      .last = nums[nr_atoms - 1],
      .fence_fd = -1,
      .nr_atoms = nr_atoms,
   };
   memcpy(sub->atoms, nums, nr_atoms);
   *seqno = sub->seqno;
   return VK_SUCCESS;
}

static VkResult
panvk_kbase_wait_seqno(struct panvk_kbase_jm *k, uint64_t seqno,
                       uint64_t abs_timeout_ns)
{
   VkResult result = VK_SUCCESS;

   mtx_lock(&k->mtx);
   if (k->lost)
      result = VK_ERROR_DEVICE_LOST;
   while (k->completed_seqno < seqno) {
      if (k->lost) {
         result = VK_ERROR_DEVICE_LOST;
         break;
      }
      if (!panvk_kbase_progress_locked(k, abs_timeout_ns) &&
          k->completed_seqno < seqno) {
         result = VK_TIMEOUT;
         break;
      }
   }
   if (k->lost)
      result = VK_ERROR_DEVICE_LOST;
   mtx_unlock(&k->mtx);
   return result;
}

/* Submit what the chain holds, plus its pending wait, as one submission. */
static VkResult
panvk_kbase_flush(struct panvk_kbase_jm *k, struct panvk_kbase_chain *c,
                  bool wait, bool limit, uint64_t *out_seqno)
{
   uint64_t seqno = 0;

   mtx_lock(&k->mtx);
   k->stats.chain_flushes++;
   k->stats.chain_jobs += c->nr_jobs;
   k->stats.chain_batches += c->nr_batches;
   if (k->trace && k->trace_limit_pending && !limit) {
      k->stats.limit_tail_flushes++;
      k->stats.limit_tail_jobs += c->nr_jobs;
      k->stats.limit_tail_batches += c->nr_batches;
      k->trace_limit_pending = false;
   }
   if (limit) {
      k->stats.limit_flushes++;
      k->stats.limit_jobs += c->nr_jobs;
      k->stats.limit_batches += c->nr_batches;
      k->trace_limit_pending = k->trace;
   }
   VkResult result =
      panvk_kbase_submit_locked(k, c->jobs, c->nr_jobs, c->wait_fd, &seqno);
   mtx_unlock(&k->mtx);

   /* kbase took its own reference to the fence during the ioctl. */
   if (c->wait_fd >= 0)
      close(c->wait_fd);
   c->wait_fd = -1;

   if (result == VK_SUCCESS) {
      for (unsigned i = 0; i < c->nr_batches; i++)
         c->batches[i]->kbase_seqno = seqno;
   }
   c->nr_jobs = 0;
   c->nr_batches = 0;

   if (result == VK_SUCCESS && wait)
      result = panvk_kbase_wait_seqno(k, seqno, OS_TIMEOUT_INFINITE);
   if (out_seqno)
      *out_seqno = seqno;
   return result;
}

/* One job chain as its own submission, waited for (the per-chain path). */
static VkResult
panvk_kbase_submit_one(struct panvk_kbase_jm *k, uint64_t jc,
                       uint32_t core_req, uint64_t *seqno)
{
   const struct panvk_kbase_job job = {.jc = jc, .core_req = core_req};

   mtx_lock(&k->mtx);
   VkResult result = panvk_kbase_submit_locked(k, &job, 1, -1, seqno);
   mtx_unlock(&k->mtx);
   if (result != VK_SUCCESS)
      return result;
   return panvk_kbase_wait_seqno(k, *seqno, OS_TIMEOUT_INFINITE);
}

/* panvk_kbase_sync_wait_func for syncs signalled by a submission. */
static VkResult
panvk_kbase_sync_wait_cb(void *data,
                         const uint64_t targets[PANVK_KBASE_SYNC_TARGET_COUNT],
                         uint64_t abs_timeout_ns)
{
   return panvk_kbase_wait_seqno(data, targets[0], abs_timeout_ns);
}

/* panvk_kbase_sync_export_func: a FENCE_TRIGGER atom after the submission,
 * one per submission (later exports get a dup of the same sync_file). */
static VkResult
panvk_kbase_sync_export_cb(
   void *data, const uint64_t targets[PANVK_KBASE_SYNC_TARGET_COUNT],
   int *sync_file)
{
   struct panvk_kbase_jm *k = data;
   const uint64_t seqno = targets[0];
   VkResult result = VK_SUCCESS;
   bool cpu_wait = k->cpu_fence_export;

   *sync_file = -1;
   mtx_lock(&k->mtx);
   if (k->lost) {
      mtx_unlock(&k->mtx);
      return VK_ERROR_DEVICE_LOST;
   }
   if (k->ctrl_export_early) {
      mtx_unlock(&k->mtx);
      return VK_SUCCESS;
   }
   while (!cpu_wait && k->completed_seqno < seqno) {
      struct panvk_kbase_sub *sub =
         &k->subs[(k->head + (seqno - k->subs[k->head].seqno)) %
                  PANVK_KBASE_MAX_SUBS];
      assert(k->count && sub->seqno == seqno);

      if (sub->fence_fd < 0) {
         uint8_t num;
         result = panvk_kbase_alloc_atoms(k, 1, &num, PANVK_KBASE_ATOM_AUX);
         if (result != VK_SUCCESS)
            break;
         /* The allocation may have waited: the submission may have completed,
          * or another export may have made its fence. Look again. */
         if (k->completed_seqno >= seqno || sub->fence_fd >= 0) {
            panvk_kbase_free_atoms(k, 1, &num);
            continue;
         }

         struct base_fence fence = {.basep = {.fd = -1, .stream_fd = -1}};
         base_jd_atom atom;
         panvk_kbase_atom(&atom, num, (uintptr_t)&fence,
                          BASE_JD_REQ_SOFT_FENCE_TRIGGER, sub->last,
                          BASE_JD_DEP_TYPE_DATA, 0, 0);
         struct kbase_ioctl_job_submit submit = {
            .addr = (uint64_t)(uintptr_t)&atom,
            .nr_atoms = 1,
            .stride = PANVK_KBASE_ATOM_STRIDE,
         };
         if (ioctl(k->fd, KBASE_IOCTL_JOB_SUBMIT, &submit)) {
            mesa_loge("panvk: kbase FENCE_TRIGGER submit failed: %s",
                      strerror(errno));
            if (fence.basep.fd >= 0)
               close(fence.basep.fd);
            k->lost = true;
            u_cnd_monotonic_broadcast(&k->cnd);
            result = VK_ERROR_DEVICE_LOST;
            break;
         }
         if (fence.basep.fd < 0) {
            /* The atom completes (and frees its number) with an error. */
            mesa_loge("panvk: kbase FENCE_TRIGGER gave no fd");
            cpu_wait = true;
            break;
         }
         sub->fence_fd = fence.basep.fd;
         k->stats.export_new++;
      } else {
         k->stats.export_dup++;
      }

      *sync_file = os_dupfd_cloexec(sub->fence_fd);
      if (*sync_file < 0)
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
      break;
   }
   if (cpu_wait)
      k->stats.export_cpu++;
   else if (*sync_file < 0 && result == VK_SUCCESS)
      k->stats.export_done++;
   mtx_unlock(&k->mtx);

   if (cpu_wait && result == VK_SUCCESS)
      result = panvk_kbase_wait_seqno(k, seqno, OS_TIMEOUT_INFINITE);
   return result;
}

static bool
panvk_kbase_chain_has_batch(const struct panvk_kbase_chain *c,
                            const struct panvk_batch *batch)
{
   for (unsigned i = 0; i < c->nr_batches; i++) {
      if (c->batches[i] == batch)
         return true;
   }
   return false;
}

static VkResult
panvk_queue_submit_batch(struct panvk_gpu_queue *queue,
                         struct panvk_cmd_buffer *cmdbuf,
                         struct panvk_batch *batch, uint32_t *bos,
                         unsigned nr_bos, uint32_t *in_fences,
                         unsigned nr_in_fences, struct panvk_kbase_chain *chain)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   struct panvk_physical_device *phys_dev =
      to_panvk_physical_device(dev->vk.physical);
   struct panvk_kbase_jm *k = queue->kjm;
   VkResult result;
   int ret;

   /* Reset the batch if it's already been issued */
   if (batch->issued) {
      /* Never rewrite a batch that is still queued in the current chain, or
       * that the GPU may still be running. */
      if (chain && panvk_kbase_chain_has_batch(chain, batch)) {
         result = panvk_kbase_flush(k, chain, false, false, NULL);
         if (result != VK_SUCCESS)
            return result;
      }
      result = panvk_kbase_wait_seqno(k, batch->kbase_seqno,
                                      OS_TIMEOUT_INFINITE);
      if (result != VK_SUCCESS)
         return result;

      util_dynarray_foreach(&batch->jobs, void *, job)
         memset((*job), 0, 4 * 4);

      /* Reset the tiler before re-issuing the batch */
      if (batch->tiler.ctx_descs.cpu) {
         memcpy(batch->tiler.heap_desc.cpu, &batch->tiler.heap_templ,
                sizeof(batch->tiler.heap_templ));

         struct mali_tiler_context_packed *ctxs = batch->tiler.ctx_descs.cpu;

         for (uint32_t i = 0; i < batch->fb.layer_count; i++)
            memcpy(&ctxs[i], &batch->tiler.ctx_templ, sizeof(*ctxs));
      }

      /* We don't keep track of BO <-> job relationship, so let's just flush the
       * whole desc pool for now. */
      panvk_pool_flush_maps(&cmdbuf->desc_pool);
   }

   /* Flush pending synchronization requests before submitting the job, to
    * make sure things are GPU-visible. */
   pan_kmod_flush_bo_map_syncs(dev->kmod.dev);

   if (chain) {
      if (chain->nr_jobs + 2 > PANVK_KBASE_MAX_CHAIN ||
          chain->nr_batches + 1 > PANVK_KBASE_MAX_CHAIN) {
         result = panvk_kbase_flush(k, chain, false, true, NULL);
         if (result != VK_SUCCESS)
            return result;
      }
      if (batch->vtc_jc.first_job)
         chain->jobs[chain->nr_jobs++] = (struct panvk_kbase_job){
            .jc = batch->vtc_jc.first_job, .core_req = 0x16};
      if (batch->frag_jc.first_job)
         chain->jobs[chain->nr_jobs++] = (struct panvk_kbase_job){
            .jc = batch->frag_jc.first_job, .core_req = 0x01};
      chain->batches[chain->nr_batches++] = batch;
      batch->issued = true;
      return VK_SUCCESS;
   }

   /* SKIP: dump sebelum submit dipastikan SIGSEGV karena ALLOC_ON_FAULT (commit_pages=0), nggak informatif */
   if (batch->vtc_jc.first_job) {
      result = panvk_kbase_submit_one(k, batch->vtc_jc.first_job, 0x16,
                                      &batch->kbase_seqno); /* PATCH eksperimen: dulu 0x16 (CS|T|V) */
      ret = result == VK_SUCCESS ? 0 : -1;
      if (ret)
         mesa_loge("panvk: v9 vtc_jc job chain failed: %d "
                   "(PANVK_V9_TRACE=1 dumps the job status)", result);
      if (result != VK_SUCCESS)
         return result;
      if (pan_v9_trace_enabled() || getenv("PANVK_HEAP_LOG")) {
         fprintf(panvk_debug_log_file(),
                 "[PANVK_DEBUG_SUBMIT] vtc_jc ret=%d\n", ret);
         panvk_debug_dump_job_status(cmdbuf, batch, "vtc_jc");
         panvk_debug_dump_tiler_heap(dev, "SESUDAH vtc_jc (vertex+tiler)");
      }

      if (PANVK_DEBUG(TRACE)) {
         panvk_pool_invalidate_maps(&cmdbuf->desc_pool);
      }

      if (PANVK_DEBUG(TRACE)) {
         pandecode_jc(dev->debug.decode_ctx, batch->vtc_jc.first_job,
                      phys_dev->kmod.dev->props.gpu_id);
      }

      if (PANVK_DEBUG(DUMP))
         pandecode_dump_mappings(dev->debug.decode_ctx);

   }

   if (batch->frag_jc.first_job) {
      result = panvk_kbase_submit_one(k, batch->frag_jc.first_job, 0x01,
                                      &batch->kbase_seqno);
      ret = result == VK_SUCCESS ? 0 : -1;
      if (ret)
         mesa_loge("panvk: v9 frag_jc job chain failed: %d "
                   "(PANVK_V9_TRACE=1 dumps the job status)", result);
      if (result != VK_SUCCESS)
         return result;
      if (pan_v9_trace_enabled() || getenv("PANVK_HEAP_LOG")) {
         fprintf(panvk_debug_log_file(),
                 "[PANVK_DEBUG_SUBMIT] frag_jc ret=%d\n", ret);
         panvk_debug_dump_job_status(cmdbuf, batch, "frag_jc");
         panvk_debug_dump_tiler_heap(dev, "SESUDAH frag_jc (fragment)");
      }
      if (PANVK_DEBUG(TRACE)) {
         /* If we want to read the descriptors back, we need to invalidate the
          * whole desc pool, otherwise we might end up with stale data. */
         panvk_pool_invalidate_maps(&cmdbuf->desc_pool);
         pan_kmod_flush_bo_map_syncs(dev->kmod.dev);
      }

      if (PANVK_DEBUG(TRACE))
         pandecode_jc(dev->debug.decode_ctx, batch->frag_jc.first_job,
                      phys_dev->kmod.dev->props.gpu_id);

      if (PANVK_DEBUG(DUMP))
         pandecode_dump_mappings(dev->debug.decode_ctx);

   }

   if (PANVK_DEBUG(TRACE))
      pandecode_next_frame(dev->debug.decode_ctx);

   batch->issued = true;
   return VK_SUCCESS;
}

static void
panvk_queue_transfer_sync(struct panvk_gpu_queue *queue, uint32_t syncobj)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   ASSERTED int ret;

   struct drm_syncobj_handle handle = {
      .handle = queue->sync,
      .flags = DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE,
      .fd = -1,
   };

   ret = pan_kmod_ioctl(dev->drm_fd, DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, &handle);
   assert(!ret);
   assert(handle.fd >= 0);

   handle.handle = syncobj;
   ret = pan_kmod_ioctl(dev->drm_fd, DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, &handle);
   assert(!ret);

   close(handle.fd);
}

static void
panvk_add_wait_event_syncobjs(struct panvk_batch *batch, uint32_t *in_fences,
                              unsigned *nr_in_fences)
{
   util_dynarray_foreach(&batch->event_ops, struct panvk_cmd_event_op, op) {
      switch (op->type) {
      case PANVK_EVENT_OP_SET:
         /* Nothing to do yet */
         break;
      case PANVK_EVENT_OP_RESET:
         /* Nothing to do yet */
         break;
      case PANVK_EVENT_OP_WAIT:
         /* kbase: the set happened in an earlier, already finished batch
          * or on the host before submission (required by the spec). */
         if (!op->event->kbase)
            in_fences[(*nr_in_fences)++] = op->event->syncobj;
         break;
      default:
         UNREACHABLE("bad panvk_cmd_event_op type\n");
      }
   }
}

static void
panvk_signal_event_syncobjs(struct panvk_gpu_queue *queue,
                            struct panvk_batch *batch)
{
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);

   util_dynarray_foreach(&batch->event_ops, struct panvk_cmd_event_op, op) {
      switch (op->type) {
      case PANVK_EVENT_OP_SET: {
         if (op->event->kbase) {
            /* The batch has completed (synchronous kbase submit).
             * PANVK_KBASE_EVENT_NO_SET=1: negative control, drop the set. */
            if (!getenv("PANVK_KBASE_EVENT_NO_SET"))
               p_atomic_set(&op->event->kbase_set, 1);
            break;
         }
         panvk_queue_transfer_sync(queue, op->event->syncobj);
         break;
      }
      case PANVK_EVENT_OP_RESET: {
         struct panvk_event *event = op->event;
         if (event->kbase) {
            p_atomic_set(&event->kbase_set, 0);
            break;
         }

         struct drm_syncobj_array objs = {
            .handles = (uint64_t)(uintptr_t)&event->syncobj,
            .count_handles = 1};

         ASSERTED int ret = pan_kmod_ioctl(dev->drm_fd,
                                  DRM_IOCTL_SYNCOBJ_RESET, &objs);
         assert(!ret);
         break;
      }
      case PANVK_EVENT_OP_WAIT:
         /* Nothing left to do */
         break;
      default:
         UNREACHABLE("bad panvk_cmd_event_op type\n");
      }
   }
}

VkResult
panvk_per_arch(gpu_queue_submit)(struct vk_queue *vk_queue, struct vk_queue_submit *submit)
{
   struct panvk_gpu_queue *queue = container_of(vk_queue, struct panvk_gpu_queue, vk);
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   struct panvk_kbase_jm *k = queue->kjm;
   /* One chain per submit; the per-chain path (8.9's control, trace, dump)
    * waits for every chain, so it keeps the 8.14 waits and signals too. */
   struct panvk_kbase_chain *chain =
      panvk_kbase_chain_enabled() ? &k->chain : NULL;
   const bool wait_submit = k->wait_submit || !chain;
   VkResult result;

   unsigned nr_semaphores = submit->wait_count + 1;
   uint32_t semaphores[nr_semaphores];

   /* Waits. Work submitted earlier on this queue needs nothing: kbase runs
    * every submission after the previous one. Imported sync_files are merged
    * into one FENCE_WAIT atom, so the GPU waits for them. Anything else (and
    * everything under PANVK_KBASE_WAIT_SUBMIT) is waited for on the CPU
    * before submitting, as before 8.15. */
   int wait_fd = -1;
   semaphores[0] = 0;
   for (unsigned i = 0; i < submit->wait_count; i++) {
      struct vk_sync *sync = submit->waits[i].sync;
      uint64_t targets[PANVK_KBASE_SYNC_TARGET_COUNT];
      void *data = NULL;
      int fd = -1;
      enum panvk_kbase_sync_payload payload =
         wait_submit ? PANVK_KBASE_SYNC_PAYLOAD_OTHER
                     : panvk_kbase_sync_get_payload(sync, &data, targets, &fd);

      semaphores[i + 1] = 0;
      if (payload == PANVK_KBASE_SYNC_PAYLOAD_NONE) {
         k->stats.wait_none++;
         continue;
      }
      if (payload == PANVK_KBASE_SYNC_PAYLOAD_GPU && data == k) {
         k->stats.wait_own++;
         continue;
      }
      if (payload == PANVK_KBASE_SYNC_PAYLOAD_FILE) {
         int ret = k->cpu_fence_wait ? -1
                                     : sync_accumulate("panvk", &wait_fd, fd);
         close(fd);
         if (!ret) {
            k->stats.wait_file++;
            continue;
         }
      }
      k->stats.wait_cpu++;

      VkResult wr = vk_sync_wait(&dev->vk, sync, submit->waits[i].wait_value,
                                 VK_SYNC_WAIT_COMPLETE, UINT64_MAX);
      if (wr != VK_SUCCESS) {
         if (wait_fd >= 0)
            close(wait_fd);
         if (wr == VK_ERROR_DEVICE_LOST)
            return vk_device_set_lost(&dev->vk, "kbase sync wait failed");
         return wr;
      }
   }
   if (chain)
      chain->wait_fd = wait_fd;
   else
      assert(wait_fd < 0);

   for (uint32_t j = 0; j < submit->command_buffer_count; ++j) {
      struct panvk_cmd_buffer *cmdbuf =
         container_of(submit->command_buffers[j], struct panvk_cmd_buffer, vk);

      list_for_each_entry(struct panvk_batch, batch, &cmdbuf->batches, node) {
         /* FIXME: should be done at the batch level */
         unsigned nr_bos = panvk_pool_num_bos(&cmdbuf->desc_pool) +
                           panvk_pool_num_bos(&cmdbuf->varying_pool) +
                           panvk_pool_num_bos(&cmdbuf->tls_pool) +
                           batch->fb.bo_count + (batch->blit.src ? 1 : 0) +
                           (batch->blit.dst ? 1 : 0) +
                           (batch->vtc_jc.first_tiler ? 1 : 0) + 1;
         unsigned bo_idx = 0;
         uint32_t bos[nr_bos];

         panvk_pool_get_bo_handles(&cmdbuf->desc_pool, &bos[bo_idx]);
         bo_idx += panvk_pool_num_bos(&cmdbuf->desc_pool);

         panvk_pool_get_bo_handles(&cmdbuf->varying_pool, &bos[bo_idx]);
         bo_idx += panvk_pool_num_bos(&cmdbuf->varying_pool);

         panvk_pool_get_bo_handles(&cmdbuf->tls_pool, &bos[bo_idx]);
         bo_idx += panvk_pool_num_bos(&cmdbuf->tls_pool);

         for (unsigned i = 0; i < batch->fb.bo_count; i++)
            bos[bo_idx++] = pan_kmod_bo_handle(batch->fb.bos[i]);

         if (batch->blit.src)
            bos[bo_idx++] = pan_kmod_bo_handle(batch->blit.src);

         if (batch->blit.dst)
            bos[bo_idx++] = pan_kmod_bo_handle(batch->blit.dst);

         if (batch->vtc_jc.first_tiler)
            bos[bo_idx++] = pan_kmod_bo_handle(dev->tiler_heap->bo);

         bos[bo_idx++] = pan_kmod_bo_handle(dev->sample_positions->bo);
         assert(bo_idx == nr_bos);

         /* Merge identical BO entries. */
         for (unsigned x = 0; x < nr_bos; x++) {
            for (unsigned y = x + 1; y < nr_bos;) {
               if (bos[x] == bos[y])
                  bos[y] = bos[--nr_bos];
               else
                  y++;
            }
         }

         unsigned nr_in_fences = 0;
         unsigned max_wait_event_syncobjs = util_dynarray_num_elements(
            &batch->event_ops, struct panvk_cmd_event_op);
         uint32_t in_fences[nr_semaphores + max_wait_event_syncobjs];
         memcpy(in_fences, semaphores, nr_semaphores * sizeof(*in_fences));
         nr_in_fences += nr_semaphores;

         panvk_add_wait_event_syncobjs(batch, in_fences, &nr_in_fences);

         /* Event ops are applied on the CPU around a batch that has finished:
          * submit what precedes it, then the batch alone, and wait for it
          * before its sets/resets are applied. */
         const bool has_event_ops = max_wait_event_syncobjs > 0;
         if (chain && has_event_ops &&
             (result = panvk_kbase_flush(k, chain, false, false, NULL)))
            goto fail;

         result = panvk_queue_submit_batch(queue, cmdbuf, batch, bos, nr_bos,
                                           in_fences, nr_in_fences, chain);
         if (result != VK_SUCCESS)
            goto fail;

         if (chain && has_event_ops &&
             (result = panvk_kbase_flush(k, chain, true, false, NULL)))
            goto fail;

         panvk_signal_event_syncobjs(queue, batch);
      }
   }

   /* The rest of the chain (or, with no command buffers, only the waits). */
   uint64_t seqno = 0;
   if (chain) {
      result = panvk_kbase_flush(k, chain, wait_submit, false, &seqno);
      if (result != VK_SUCCESS)
         goto fail;
   } else {
      mtx_lock(&k->mtx);
      seqno = k->last_seqno;
      mtx_unlock(&k->mtx);
   }

   /* Signal fences and semaphores. Until 8.15 all work had finished here and
    * the signal was raised on the CPU (the fork's first version transferred a
    * DRM syncobj, which fails on a kbase fd: PANVK_KBASE_NO_SIGNAL=1 keeps
    * that broken behaviour as a negative control). Now a signalled sync is
    * pending on the submission's seqno: CPU waits block on completion, and
    * a SYNC_FD export becomes a FENCE_TRIGGER atom. */
   if (!getenv("PANVK_KBASE_NO_SIGNAL")) {
      const uint64_t targets[PANVK_KBASE_SYNC_TARGET_COUNT] = {seqno};

      for (unsigned i = 0; i < submit->signal_count; i++) {
         if (wait_submit || !seqno) {
            VkResult sr = vk_sync_signal(&dev->vk, submit->signals[i].sync,
                                         submit->signals[i].signal_value);
            if (sr != VK_SUCCESS) {
               result = sr;
               goto fail;
            }
         } else {
            panvk_kbase_sync_set_pending(submit->signals[i].sync, k,
                                         panvk_kbase_sync_wait_cb,
                                         panvk_kbase_sync_export_cb, targets);
         }
      }
   }

   return VK_SUCCESS;

fail:
   if (chain) {
      if (chain->wait_fd >= 0)
         close(chain->wait_fd);
      chain->wait_fd = -1;
      chain->nr_jobs = 0;
      chain->nr_batches = 0;
   }
   if (result == VK_ERROR_DEVICE_LOST)
      return vk_device_set_lost(&dev->vk, "kbase submission failed");
   return result;
}

static void
panvk_kbase_log_stats(struct panvk_kbase_jm *k)
{
   if (!k->trace)
      return;
   mtx_lock(&k->mtx);
   mesa_logi("panvk: kbase submissions %" PRIu64 "; waits: none %" PRIu64
             " own %" PRIu64 " sync_file %" PRIu64 " cpu %" PRIu64
             "; FENCE_WAIT atoms %" PRIu64 "; exports: new %" PRIu64
             " dup %" PRIu64 " done %" PRIu64 " cpu %" PRIu64,
             k->last_seqno, k->stats.wait_none, k->stats.wait_own,
             k->stats.wait_file, k->stats.wait_cpu, k->stats.fence_wait_atoms,
             k->stats.export_new, k->stats.export_dup, k->stats.export_done,
             k->stats.export_cpu);
   mesa_logi(
      "panvk: kbase trace atom_allocated=%" PRIu64 " atom_freed=%" PRIu64
      " atom_reused=%" PRIu64 " cursor_wraps=%" PRIu64
      " alloc_pressure_waits=%" PRIu64 " alloc_pressure_iterations=%" PRIu64
      " max_live_atoms=%" PRIu64 " events=%" PRIu64 " failed_events=%" PRIu64
      " retired_submissions=%" PRIu64 " chain_flushes=%" PRIu64
      " chain_jobs=%" PRIu64 " chain_batches=%" PRIu64 " limit_flushes=%" PRIu64
      " limit_jobs=%" PRIu64 " limit_batches=%" PRIu64
      " limit_tail_flushes=%" PRIu64 " limit_tail_jobs=%" PRIu64
      " limit_tail_batches=%" PRIu64,
      k->stats.atom_allocated, k->stats.atom_freed, k->stats.atom_reused,
      k->stats.cursor_wraps, k->stats.alloc_pressure_waits,
      k->stats.alloc_pressure_iterations, k->stats.max_live_atoms,
      k->stats.events, k->stats.failed_events, k->stats.retired_submissions,
      k->stats.chain_flushes, k->stats.chain_jobs, k->stats.chain_batches,
      k->stats.limit_flushes, k->stats.limit_jobs, k->stats.limit_batches,
      k->stats.limit_tail_flushes, k->stats.limit_tail_jobs,
      k->stats.limit_tail_batches);
   mtx_unlock(&k->mtx);
}

static VkResult
panvk_kbase_jm_create(struct panvk_device *device, struct panvk_kbase_jm **out)
{
   struct panvk_kbase_jm *k =
      vk_zalloc(&device->vk.alloc, sizeof(*k), 8,
                VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!k)
      return panvk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   k->fd = device->drm_fd;
   k->chain.wait_fd = -1;
   /* Atom number 0 means "no dependency": never hand it out. */
   k->atom_use[0] = PANVK_KBASE_ATOM_SUB;
   k->nr_free = 255;
   k->cursor = 1;
   k->wait_submit = os_get_option("PANVK_KBASE_WAIT_SUBMIT") != NULL;
   k->cpu_fence_wait = os_get_option("PANVK_KBASE_CPU_FENCE_WAIT") != NULL;
   k->cpu_fence_export = os_get_option("PANVK_KBASE_CPU_FENCE_EXPORT") != NULL;
   k->trace = os_get_option("PANVK_KBASE_FENCE_TRACE") != NULL;
   k->ctrl_no_fence_wait_dep =
      os_get_option("PANVK_KBASE_CTRL_NO_FENCE_WAIT_DEP") != NULL;
   k->ctrl_export_early = os_get_option("PANVK_KBASE_CTRL_EXPORT_EARLY") != NULL;
#ifdef PANVK_JM_FAULT_INJECTION
   if (!panvk_jm_fault_parser_self_test()) {
      mesa_loge("panvk: jm-fault state=SELF_TEST_FAILED");
      goto err_free;
   }
   const char *fault_selector = os_get_option("PANVK_JM_FAULT");
   if (fault_selector && !panvk_jm_fault_parse(fault_selector, &k->fault)) {
      mesa_loge("panvk: jm-fault state=REJECTED");
      goto err_free;
   }
#endif

   if (mtx_init(&k->mtx, mtx_plain) != thrd_success)
      goto err_free;
   if (u_cnd_monotonic_init(&k->cnd) != thrd_success)
      goto err_mtx;

#ifdef PANVK_JM_FAULT_INJECTION
   if (k->fault.mode != PANVK_JM_FAULT_NONE)
      panvk_jm_fault_log(k, "ARMED", 0);
#endif

   *out = k;
   return VK_SUCCESS;

err_mtx:
   mtx_destroy(&k->mtx);
err_free:
   vk_free(&device->vk.alloc, k);
   return panvk_error(device, VK_ERROR_INITIALIZATION_FAILED);
}

static void
panvk_kbase_jm_destroy(struct panvk_device *device, struct panvk_kbase_jm *k)
{
   if (!k)
      return;

   /* The app must have waited, but never free what the GPU may still use;
    * a hung GPU gets a bounded wait (kbase stops hung jobs by itself). */
   uint64_t seqno;
   mtx_lock(&k->mtx);
#ifdef PANVK_JM_FAULT_INJECTION
   if (k->fault.mode != PANVK_JM_FAULT_NONE &&
       !atomic_load_explicit(&k->fault.consumed, memory_order_acquire)) {
      panvk_jm_fault_log(k, "UNCONSUMED", 0);
      atomic_store_explicit(&k->fault.remaining, 0, memory_order_relaxed);
   }
#endif
   seqno = k->last_seqno;
   mtx_unlock(&k->mtx);
   if (panvk_kbase_wait_seqno(k, seqno, os_time_get_absolute_timeout(
                                           10ull * 1000 * 1000 * 1000)))
      mesa_loge("panvk: kbase queue not idle at destroy");

   panvk_kbase_log_stats(k);

   for (unsigned i = 0; i < k->count; i++) {
      struct panvk_kbase_sub *sub =
         &k->subs[(k->head + i) % PANVK_KBASE_MAX_SUBS];
      if (sub->fence_fd >= 0)
         close(sub->fence_fd);
   }
   if (k->chain.wait_fd >= 0)
      close(k->chain.wait_fd);
   u_cnd_monotonic_destroy(&k->cnd);
   mtx_destroy(&k->mtx);
   vk_free(&device->vk.alloc, k);
}

VkResult
panvk_per_arch(create_gpu_queue)(struct panvk_device *device,
                                 const VkDeviceQueueCreateInfo *create_info,
                                 uint32_t queue_idx,
                                 struct vk_queue **out_queue)
{
   ASSERTED const VkDeviceQueueGlobalPriorityCreateInfoKHR *priority_info =
      vk_find_struct_const(create_info->pNext,
                           DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR);
   ASSERTED const VkQueueGlobalPriorityKHR priority =
      priority_info ? priority_info->globalPriority
                    : VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR;

   /* XXX: Panfrost kernel module doesn't support priorities so far */
   assert(priority == VK_QUEUE_GLOBAL_PRIORITY_MEDIUM_KHR);

   struct panvk_gpu_queue *queue = vk_zalloc(&device->vk.alloc, sizeof(*queue), 8,
                                         VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!queue)
      return panvk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);

   VkResult result =
      vk_queue_init(&queue->vk, &device->vk, create_info, queue_idx);
   if (result != VK_SUCCESS)
      goto err_free_queue;

   queue->sync = 0;

   /* Atom numbers belong to the kbase context (one per VkDevice), and v9
    * exposes a single queue: the state lives with it. */
   result = panvk_kbase_jm_create(device, &queue->kjm);
   if (result != VK_SUCCESS)
      goto err_finish_queue;

   queue->vk.driver_submit = panvk_per_arch(gpu_queue_submit);
   *out_queue = &queue->vk;
   return VK_SUCCESS;

err_finish_queue:
   vk_queue_finish(&queue->vk);

err_free_queue:
   vk_free(&device->vk.alloc, queue);
   return result;
}

void panvk_per_arch(destroy_gpu_queue)(struct vk_queue *vk_queue)
{
   struct panvk_gpu_queue *queue = container_of(vk_queue, struct panvk_gpu_queue, vk);
   struct panvk_device *dev = to_panvk_device(vk_queue->base.device);

   vk_queue_finish(&queue->vk);
   panvk_kbase_jm_destroy(dev, queue->kjm);
   /* PATCH: no drm_syncobj to destroy */
   vk_free(&dev->vk.alloc, queue);
}

VkResult
panvk_per_arch(gpu_queue_check_status)(struct vk_queue *vk_queue)
{
   struct panvk_gpu_queue *queue =
      container_of(vk_queue, struct panvk_gpu_queue, vk);
   struct panvk_device *dev = to_panvk_device(vk_queue->base.device);
   struct panvk_kbase_jm *k = queue->kjm;

   mtx_lock(&k->mtx);
   while (!k->lost && panvk_kbase_progress_locked(k, 0))
      ;
   bool lost = k->lost;
   mtx_unlock(&k->mtx);

   if (lost)
      return vk_device_set_lost(&dev->vk, "kbase queue lost");
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
panvk_per_arch(QueueWaitIdle)(VkQueue _queue)
{
   VK_FROM_HANDLE(panvk_gpu_queue, queue, _queue);
   struct panvk_device *dev = to_panvk_device(queue->vk.base.device);
   struct panvk_kbase_jm *k = queue->kjm;

   /* we need to use vk_common_QueueWaitIdle if we ever go threaded */
   assert(queue->vk.submit.mode != VK_QUEUE_SUBMIT_MODE_THREADED);

   if (vk_device_is_lost(&dev->vk)) {
      /* Check printf buffer one more time before exiting */
      u_printf_with_ctx(stdout, &dev->printf.ctx);
      return VK_ERROR_DEVICE_LOST;
   }

   /* Submits no longer wait for their work (8.15): wait for the newest. */
   uint64_t seqno;
   mtx_lock(&k->mtx);
   seqno = k->last_seqno;
   mtx_unlock(&k->mtx);
   VkResult result = panvk_kbase_wait_seqno(k, seqno, OS_TIMEOUT_INFINITE);
   if (result != VK_SUCCESS)
      return vk_device_set_lost(&dev->vk, "kbase queue wait failed: %d",
                                result);
   panvk_kbase_log_stats(k);

   return VK_SUCCESS;
}
