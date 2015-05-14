
#include <stdio.h>
#include <string.h>
#if FALSE // iOS
#include <sys/time.h>
#include <mach/mach_time.h>
#else
#include <time.h>
#include <windows.h>
#endif

#include <mono/profiler/HeapBoss/OutFileWriter.hpp>

//extern "C"
//{
#include <glib.h>
//#include <mono/metadata/domain-internals.h>

#if G_BYTE_ORDER == G_LITTLE_ENDIAN
#   define GINT32_TO_LE(x) (x)
#   define GINT64_TO_LE(x) (x)
#   define GINT16_TO_LE(x) (x)
#   define GINT_TO_LE(x)   (x)
#   define GINT32_TO_BE(x) GUINT32_SWAP_LE_BE(x)
#   define GINT32_FROM_BE(x) GUINT32_SWAP_LE_BE(x)
#else
#   define GINT32_TO_LE(x) GUINT32_SWAP_LE_BE(x)
#   define GINT64_TO_LE(x) GUINT64_SWAP_LE_BE(x)
#   define GINT16_TO_LE(x) GUINT16_SWAP_LE_BE(x)
#   define GINT_TO_LE(x)   GUINT32_SWAP_LE_BE(x)
#   define GINT32_TO_BE(x) (x)
#   define GUINT32_FROM_BE(x) (x)
#endif

#define GINT32_FROM_LE(x)  (GINT32_TO_LE (x))
#define GINT64_FROM_LE(x)  (GINT64_TO_LE (x))
#define GINT16_FROM_LE(x)  (GINT16_TO_LE (x))
#define GINT_FROM_LE(x)    (GINT_TO_LE (x))

//} // extern C

static void write_byte(FILE *out, guint8 x)
{
	fwrite (&x, sizeof (x), 1, out);
}

template<typename TUInt>
static void write_vuint(FILE *out, TUInt x)
{
	guint8 y;
	
	do {
		y = (guint8) (x & 0x7f);
		x = x >> 7;
		if (x != 0)
			y |= 0x80;
		write_byte (out, y);
	} while (x != 0);
}

static void write_int16(FILE *out, gint16 x)
{
	x = GINT16_TO_LE(x);
	fwrite (&x, sizeof (x), 1, out);
}
static void write_uint16(FILE *out, guint16 x)
{
	x = GUINT16_TO_LE(x);
	fwrite (&x, sizeof (x), 1, out);
}

static void write_int32(FILE *out, gint32 x)
{
	x = GINT32_TO_LE(x);
	fwrite (&x, sizeof (x), 1, out);
}
static void write_uint32(FILE *out, guint32 x)
{
	x = GUINT32_TO_LE(x);
	fwrite (&x, sizeof (x), 1, out);
}

static void write_int64(FILE *out, gint64 x)
{
	x = GINT64_TO_LE(x);
	fwrite (&x, sizeof (x), 1, out);
}
static void write_uint64(FILE *out, guint64 x)
{
	x = GUINT64_TO_LE(x);
	fwrite (&x, sizeof (x), 1, out);
}

static void write_pointer(FILE *out, const void* x)
{
	//write_uint64(out, reinterpret_cast<guint64>(x));
	write_vuint(out, reinterpret_cast<guint64>(x));
}

static void write_time(FILE *out, uint64_t x)
{
	//write_uint64(out, x);
	write_vuint(out, x);
}
static void write_time_offset(FILE *out, uint64_t x)
{
	//write_uint64(out, x);
	write_vuint(out, x);
}

static void write_string(FILE *out, const char *str)
{
	size_t len = strlen(str);
	write_vuint(out, len);
	fwrite(str, sizeof (char), len, out);
}

#if PLATFORM_WIN32 // http://stackoverflow.com/a/26085827/444977
int gettimeofday(struct timeval * tp, struct timezone * tzp)
{
	g_assert(tp);

	// Note: some broken versions only have 8 trailing zero's, the correct epoch has 9 trailing zero's
	static const uint64_t EPOCH = ((uint64_t)116444736000000000ULL);

	SYSTEMTIME  system_time;
	FILETIME    file_time;
	uint64_t    time;

	GetSystemTime(&system_time);
	SystemTimeToFileTime(&system_time, &file_time);
	time = ((uint64_t)file_time.dwLowDateTime);
	time += ((uint64_t)file_time.dwHighDateTime) << 32;

	tp->tv_sec = (long)((time - EPOCH) / 10000000ULL);
	tp->tv_usec = (long)(system_time.wMilliseconds * 1000U);

	return 0;
}
#endif
static uint64_t get_ms_since_epoch()
{
	timeval tv;
	gettimeofday(&tv, NULL);
	
	uint64_t millisecondsSinceEpoch =
		(uint64_t)(tv.tv_sec) * 1000 +
		(uint64_t)(tv.tv_usec) / 1000;
	
	return millisecondsSinceEpoch;
}
static uint64_t get_nanoseconds()
{
#if FALSE // iOS
	static mach_timebase_info_data_t info;
	mach_timebase_info(&info);
	
	uint64_t now = mach_absolute_time();
	now *= info.numer;
	now /= info.denom;
	return now;
#else
	return 0;
#endif
}

static const char* cFileLabel = "heap-boss logfile";

enum {
	cFileSignature = 0x4EABB055,
	cFileVersion = 3,
	
	cTagNone = 0,
	
	cTagType,
	cTagMethod,
	cTagBackTrace,
	cTagGarbageCollect,
	cTagResize,
	cTagMonoObjectNew,
	cTagMonoObjectSizeChange,
	cTagMonoObjectGc,
	cTagHeapSize,
	cTagHeapMemoryStart,
	cTagHeapMemoryEnd,
	cTagHeapMemorySection,
	cTagHeapMemorySectionBlock,
	cTagHeapMemoryRoots,
	cTagHeapMemoryThreads,
	cTagBoehmAlloc,
	cTagBoehmFree,
	cTagMonoVTable,
	cTagMonoClassStatics,
	cTagMonoThreadTableResize,
	cTagMonoThreadStatics,
	cTagBackTraceTypeLink,
	cTagBoehmAllocStacktrace,
	
	cTagEos = UCHAR_MAX,
	
	cTagCustomEvent = cTagEos - 1,
	cTagAppResignActive = cTagEos - 2,
	cTagAppBecomeActive = cTagEos - 3,
	cTagNewFrame = cTagEos - 4,
	cTagAppMemoryStats = cTagEos - 5,
};
