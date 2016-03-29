/*
Copyright (c) 2015-2016 Advanced Micro Devices, Inc. All rights reserved.
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANNTY OF ANY KIND, EXPRESS OR
IMPLIED, INNCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANNY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER INN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR INN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#ifndef HIP_HCC_H
#define HIP_HCC_H

#include <hc.hpp>
#include "hcc_detail/hip_util.h"
#include "hcc_detail/staging_buffer.h"

#define HIP_HCC

#if defined(__HCC__) && (__hcc_workweek__ < 1502)
#error("This version of HIP requires a newer version of HCC.");
#endif

// #define USE_MEMCPYTOSYMBOL
//
//Use the new HCC accelerator_view::copy instead of am_copy
#define USE_AV_COPY 0

//#define INLINE static inline

//---
// Environment variables:

// Intended to distinguish whether an environment variable should be visible only in debug mode, or in debug+release.
//static const int debug = 0;
extern const int release;

extern int HIP_LAUNCH_BLOCKING;

extern int HIP_PRINT_ENV;
extern int HIP_TRACE_API;
extern int HIP_DB;
extern int HIP_STAGING_SIZE;   /* size of staging buffers, in KB */
extern int HIP_STAGING_BUFFERS;    // TODO - remove, two buffers should be enough.
extern int HIP_PININPLACE;
extern int HIP_STREAM_SIGNALS;  /* number of signals to allocate at stream creation */
extern int HIP_VISIBLE_DEVICES; /* Contains a comma-separated sequence of GPU identifiers */


//---
// Chicken bits for disabling functionality to work around potential issues:
extern int HIP_DISABLE_HW_KERNEL_DEP;
extern int HIP_DISABLE_HW_COPY_DEP;

extern thread_local int tls_defaultDevice;
extern thread_local hipError_t tls_lastHipError;
struct ihipStream_t;
struct ihipDevice_t;


// Color defs for debug messages:
#define KNRM  "\x1B[0m"
#define KRED  "\x1B[31m"
#define KGRN  "\x1B[32m"
#define KYEL  "\x1B[33m"
#define KBLU  "\x1B[34m"
#define KMAG  "\x1B[35m"
#define KCYN  "\x1B[36m"
#define KWHT  "\x1B[37m"

#define API_COLOR KGRN


#define HIP_HCC  

// If set, thread-safety is enforced on all stream functions.
// Stream functions will acquire a mutex before entering critical sections.
#define STREAM_THREAD_SAFE 1

// If FORCE_COPY_DEP=1 , HIP runtime will add 
// synchronization for copy commands in the same stream, regardless of command type.
// If FORCE_COPY_DEP=0 data copies of the same kind (H2H, H2D, D2H, D2D) are assumed to be implicitly ordered.
// ROCR runtime implementation currently provides this guarantee when using SDMA queues but not 
// when using shader queues.  
// TODO - measure if this matters for performance, in particular for back-to-back small copies.
// If not, we can simplify the copy dependency tracking by collapsing to a single Copy type, and always forcing dependencies for copy commands.
#define FORCE_SAMEDIR_COPY_DEP 1


// Compile debug trace mode - this prints debug messages to stderr when env var HIP_DB is set.
// May be set to 0 to remove debug if checks - possible code size and performance difference?
#define COMPILE_HIP_DB  1


// Compile HIP tracing capability.
// 0x1 = print a string at function entry with arguments.
// 0x2 = prints a simple message with function name + return code when function exits.
// 0x3 = print both.
// Must be enabled at runtime with HIP_TRACE_API
#define COMPILE_HIP_TRACE_API 0x3 


// Compile code that generates trace markers for CodeXL ATP at HIP function begin/end.
// ATP is standard CodeXL format that includes timestamps for kernels, HSA RT APIs, and HIP APIs.
#ifndef COMPILE_TRACE_MARKER
#define COMPILE_TRACE_MARKER 0
#endif


// #include CPP files to produce one object file
#define ONE_OBJECT_FILE 0


// Compile support for trace markers that are displayed on CodeXL GUI at start/stop of each function boundary.
// TODO - currently we print the trace message at the beginning. if we waited, we could also include return codes, and any values returned
// through ptr-to-args (ie the pointers allocated by hipMalloc).
#if COMPILE_TRACE_MARKER
#include "AMDTActivityLogger.h"
#define SCOPED_MARKER(markerName,group,userString) amdtScopedMarker(markerName, group, userString)
#else 
// Swallow scoped markers:
#define SCOPED_MARKER(markerName,group,userString) 
#endif


#if COMPILE_TRACE_MARKER || (COMPILE_HIP_TRACE_API & 0x1) 
#define API_TRACE(...)\
{\
    std::string s = std::string(__func__) + " (" + ToString(__VA_ARGS__) + ')';\
    if (COMPILE_HIP_DB && HIP_TRACE_API) {\
        fprintf (stderr, API_COLOR "<<hip-api: %s\n" KNRM, s.c_str());\
    }\
    SCOPED_MARKER(s.c_str(), "HIP", NULL);\
}
#else
// Swallow API_TRACE
#define API_TRACE(...)
#endif



// This macro should be called at the beginning of every HIP API.
// It initialies the hip runtime (exactly once), and
// generate trace string that can be output to stderr or to ATP file.
#define HIP_INIT_API(...) \
	std::call_once(hip_initialized, ihipInit);\
    API_TRACE(__VA_ARGS__);

#define ihipLogStatus(_hip_status) \
    ({\
        tls_lastHipError = _hip_status;\
        \
        if ((COMPILE_HIP_TRACE_API & 0x2) && HIP_TRACE_API) {\
            fprintf(stderr, "  %ship-api: %-30s ret=%2d (%s)>>\n" KNRM, (_hip_status == 0) ? API_COLOR:KRED, __func__, _hip_status, ihipErrorString(_hip_status));\
        }\
        _hip_status;\
    })




//---
//HIP_DB Debug flags:
#define DB_API    0 /* 0x01 - shortcut to enable HIP_TRACE_API on single switch */
#define DB_SYNC   1 /* 0x02 - trace synchronization pieces */
#define DB_MEM    2 /* 0x04 - trace memory allocation / deallocation */
#define DB_COPY1  3 /* 0x08 - trace memory copy commands. . */
#define DB_SIGNAL 4 /* 0x10 - trace signal pool commands */
#define DB_COPY2  5 /* 0x20 - trace memory copy commands. Detailed. */
// When adding a new debug flag, also add to the char name table below.

static const char *dbName [] =
{
    KNRM "hip-api", // not used, 
    KYEL "hip-sync",
    KCYN "hip-mem",
    KMAG "hip-copy1",
    KRED "hip-signal",
    KNRM "hip-copy2",
};

#if COMPILE_HIP_DB
#define tprintf(trace_level, ...) {\
    if (HIP_DB & (1<<(trace_level))) {\
        fprintf (stderr, "  %s:", dbName[trace_level]); \
        fprintf (stderr, __VA_ARGS__);\
        fprintf (stderr, "%s", KNRM); \
    }\
}
#else 
/* Compile to empty code */
#define tprintf(trace_level, ...) 
#endif

class ihipException : public std::exception
{
public:
    ihipException(hipError_t e) : _code(e) {};

    hipError_t _code; 
};


#ifdef __cplusplus
extern "C" {
#endif

typedef class ihipStream_t* hipStream_t;
//typedef struct hipEvent_t {
//    struct ihipEvent_t *_handle;
//} hipEvent_t;

#ifdef __cplusplus
}
#endif

const hipStream_t hipStreamNull = 0x0;


enum ihipCommand_t {
    ihipCommandCopyH2H,
    ihipCommandCopyH2D,  
    ihipCommandCopyD2H,
    ihipCommandCopyD2D,
    ihipCommandKernel,
};

static const char* ihipCommandName[] = {
    "CopyH2H", "CopyH2D", "CopyD2H", "CopyD2D", "Kernel"
};



typedef uint64_t SIGSEQNUM;

//---
// Small wrapper around signals.
// Designed to be used from stream.
// TODO-someday refactor this class so it can be stored in a vector<>
// we already store the index here so we can use for garbage collection.
struct ihipSignal_t {
    hsa_signal_t   _hsa_signal; // hsa signal handle
    int            _index;      // Index in pool, used for garbage collection.
    SIGSEQNUM      _sig_id;     // unique sequentially increasing ID.

    ihipSignal_t();
    ~ihipSignal_t();

    void release();
};


// Used to remove lock, for performance or stimulating bugs.
class FakeMutex
{
  public:
    void lock() {  }
    bool try_lock() {return true; }
    void unlock() { }
};


#if STREAM_THREAD_SAFE
typedef std::mutex StreamMutex;
#else
typedef FakeMutex StreamMutex;
#endif


// TODO - move async copy code into stream?  Stream->async-copy.
// Add PreCopy / PostCopy to manage locks?
//




// Internal stream structure.
class ihipStream_t {
public:
typedef uint64_t SeqNum_t ;

    ihipStream_t(unsigned device_index, hc::accelerator_view av, SeqNum_t id, unsigned int flags);
    ~ihipStream_t();

    // kind is hipMemcpyKind
    void copySync (void* dst, const void* src, size_t sizeBytes, unsigned kind);
    void copyAsync(void* dst, const void* src, size_t sizeBytes, unsigned kind);

    //---
    // Thread-safe accessors - these acquire / release mutex:
    bool                 preKernelCommand();
    void                 postKernelCommand(hc::completion_future &kernel_future);

    int                  preCopyCommand(ihipSignal_t *lastCopy, hsa_signal_t *waitSignal, ihipCommand_t copyType);

    void                 reclaimSignals_ts(SIGSEQNUM sigNum);
    void                 wait(bool assertQueueEmpty=false);



    // Non-threadsafe accessors - must be protected by high-level stream lock:
    SIGSEQNUM            lastCopySeqId() { return _last_copy_signal ? _last_copy_signal->_sig_id : 0; };
    ihipSignal_t *              allocSignal();


    //-- Non-racy accessors:
    // These functions access fields set at initialization time and are non-racy (so do not acquire mutex)
    ihipDevice_t *       getDevice() const;
    StreamMutex &               mutex() {return _mutex;};

    //---
    //Member vars - these are set at initialization:
    SeqNum_t                    _id;   // monotonic sequence ID
    hc::accelerator_view        _av;
    unsigned                    _flags;
private:
    void                        enqueueBarrier(hsa_queue_t* queue, ihipSignal_t *depSignal);
    void                 waitCopy(ihipSignal_t *signal);

    // The unsigned return is hipMemcpyKind
    unsigned resolveMemcpyDirection(bool srcInDeviceMem, bool dstInDeviceMem);
    void setCopyAgents(unsigned kind, ihipCommand_t *commandType, hsa_agent_t *srcAgent, hsa_agent_t *dstAgent);

    //---

    unsigned                    _device_index;
    ihipCommand_t               _last_command_type;  // type of the last command

    // signal of last copy command sent to the stream.
    // May be NULL, indicating the previous command has completley finished and future commands don't need to create a dependency.
    // Copy can be either H2D or D2H.
    ihipSignal_t                *_last_copy_signal;
    hc::completion_future       _last_kernel_future;  // Completion future of last kernel command sent to GPU.

    int                         _signalCursor;

    SIGSEQNUM                   _stream_sig_id;      // Monotonically increasing unique signal id.
    SIGSEQNUM                   _oldest_live_sig_id; // oldest live seq_id, anything < this can be allocated.
    std::deque<ihipSignal_t>    _signalPool;   // Pool of signals for use by this stream.

    StreamMutex                  _mutex;
};



//----
// Internal event structure:
enum hipEventStatus_t {
   hipEventStatusUnitialized = 0, // event is unutilized, must be "Created" before use.
   hipEventStatusCreated     = 1,
   hipEventStatusRecording   = 2, // event has been enqueued to record something.
   hipEventStatusRecorded    = 3, // event has been recorded - timestamps are valid.
} ;


// internal hip event structure.
struct ihipEvent_t {
    hipEventStatus_t       _state;

    hipStream_t           _stream;  // Stream where the event is recorded, or NULL if all streams.
    unsigned              _flags;

    hc::completion_future _marker;
    uint64_t              _timestamp;  // store timestamp, may be set on host or by marker.

    SIGSEQNUM             _copy_seq_id;
} ;





//-------------------------------------------------------------------------------------------------
struct ihipDevice_t
{
    unsigned                _device_index; // index into g_devices.

    hipDeviceProp_t         _props;        // saved device properties.
    hc::accelerator         _acc;
    hsa_agent_t             _hsa_agent;    // hsa agent handle

    // The NULL stream is used if no other stream is specified.
    // NULL has special synchronization properties with other streams.
    ihipStream_t            *_default_stream;

    std::list<ihipStream_t*> _streams;   // streams associated with this device.

    unsigned                _compute_units;

    StagingBuffer           *_staging_buffer[2]; // one buffer for each direction.

    ihipStream_t::SeqNum_t   _stream_id;

    unsigned                _device_flags;

public:
    void init(unsigned device_index, hc::accelerator acc, unsigned flags);
    ~ihipDevice_t();
    void reset();
    hipError_t getProperties(hipDeviceProp_t* prop);

    void waitAllStreams();
    void syncDefaultStream(bool waitOnSelf);

private:

};


// Global initialization.
extern std::once_flag hip_initialized;
extern ihipDevice_t *g_devices; // Array of all non-emulated (ie GPU) accelerators in the system.
extern bool g_visible_device; // Set the flag when HIP_VISIBLE_DEVICES is set
extern unsigned g_deviceCnt;
extern std::vector<int> g_hip_visible_devices; /* vector of integers that contains the visible device IDs */
extern hsa_agent_t g_cpu_agent ;   // the CPU agent.
//=================================================================================================
void ihipInit();
const char *ihipErrorString(hipError_t);
ihipDevice_t *ihipGetTlsDefaultDevice();
ihipDevice_t *ihipGetDevice(int);
void ihipSetTs(hipEvent_t e);

template<typename T>
hc::completion_future ihipMemcpyKernel(hipStream_t, T*, const T*, size_t);

template<typename T>
hc::completion_future ihipMemsetKernel(hipStream_t, T*, T, size_t);

hipStream_t ihipSyncAndResolveStream(hipStream_t);
template <typename T>

hc::completion_future
ihipMemsetKernel(hipStream_t stream, T * ptr, T val, size_t sizeBytes)
{
    int wg = std::min((unsigned)8, stream->getDevice()->_compute_units);
    const int threads_per_wg = 256;

    int threads = wg * threads_per_wg;
    if (threads > sizeBytes) {
        threads = ((sizeBytes + threads_per_wg - 1) / threads_per_wg) * threads_per_wg;
    }


    hc::extent<1> ext(threads);
    auto ext_tile = ext.tile(threads_per_wg);

    hc::completion_future cf =
    hc::parallel_for_each(
            stream->_av,
            ext_tile,
            [=] (hc::tiled_index<1> idx)
            __attribute__((hc))
    {
        int offset = amp_get_global_id(0);
        // TODO-HCC - change to hc_get_local_size()
        int stride = amp_get_local_size(0) * hc_get_num_groups(0) ;

        for (int i=offset; i<sizeBytes; i+=stride) {
            ptr[i] = val;
        }
    });

    return cf;
}

template <typename T>
hc::completion_future
ihipMemcpyKernel(hipStream_t stream, T * c, const T * a, size_t sizeBytes)
{
    int wg = std::min((unsigned)8, stream->getDevice()->_compute_units);
    const int threads_per_wg = 256;

    int threads = wg * threads_per_wg;
    if (threads > sizeBytes) {
        threads = ((sizeBytes + threads_per_wg - 1) / threads_per_wg) * threads_per_wg;
    }


    hc::extent<1> ext(threads);
    auto ext_tile = ext.tile(threads_per_wg);

    hc::completion_future cf =
    hc::parallel_for_each(
            stream->_av,
            ext_tile,
            [=] (hc::tiled_index<1> idx)
            __attribute__((hc))
    {
        int offset = amp_get_global_id(0);
        // TODO-HCC - change to hc_get_local_size()
        int stride = amp_get_local_size(0) * hc_get_num_groups(0) ;

        for (int i=offset; i<sizeBytes; i+=stride) {
            c[i] = a[i];
        }
    });

    return cf;
}

#endif