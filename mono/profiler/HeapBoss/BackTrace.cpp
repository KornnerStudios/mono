
#include <mono/profiler/HeapBoss/BackTrace.hpp>

#ifdef PLATFORM_WIN32 // because fucking mono-mutex.h doesn't include this shit
#include <windows.h>
#endif

extern "C"
{
#include <glib.h>
#include <mono/metadata/class.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/mono-debug.h>
#include <mono/io-layer/mono-mutex.h>

guint32 mono_get_jit_tls_key();
}

#include <mono/profiler/HeapBoss/HeapBoss.h>

struct HashAndCountInfo
{
	gint32 hash;
	int count;
};

static gboolean stack_walk_hash_and_count_fn(
	MonoMethod* method, gint32 native_offset, gint32 il_offset, gboolean managed, gpointer data)
{
	auto info = reinterpret_cast<HashAndCountInfo*>(data);

	const guint32 full_c = 2909;
	const guint32 method_c = 277;
	const guint32 native_c = 163;
	// stack is walked with no IL, offset will be -1
//	const guint32 il_c = 47;

	guint32 method_hash = GPOINTER_TO_UINT(method);

	info->hash = full_c * info->hash
		+ method_c * method_hash
		+ native_c * native_offset
//		+ il_c * il_offset
		+ (managed ? 1 : 0);
	info->count++;

	return false;
}

struct FrameInfo
{
	int pos;
	StackFrame **vec;
};

static gboolean stack_walk_build_frame_vector_fn(
	MonoMethod* method, gint32 native_offset, gint32 il_offset, gboolean managed, gpointer data)
{
	auto info = reinterpret_cast<FrameInfo*>(data);

	StackFrame* frame;
	frame = g_new0(StackFrame, 1);

	frame->method = method;
	frame->native_offset = native_offset;
	//frame->il_offset     = il_offset;
	//frame->managed       = managed;

	info->vec[info->pos] = frame;
	++info->pos;

	return false;
}

static void backtrace_cache_value_destroy(gpointer data)
{
	auto backtrace = reinterpret_cast<StackFrame**>(data);

	for (size_t i = 0; backtrace[i] != NULL; ++i)
	{
		g_free(backtrace[i]);
	}

	g_free(backtrace);
}
static GHashTable* backtrace_cache = NULL;
static mono_mutex_t backtrace_cache_lock /*= MONO_MUTEX_INITIALIZER*/;

void backtrace_cache_initialize()
{
	mono_mutex_init(&backtrace_cache_lock, MONO_MUTEX_INITIALIZER);
}

void backtrace_cache_dispose()
{
	g_hash_table_destroy(backtrace_cache);
	backtrace_cache = NULL;
	mono_mutex_destroy(&backtrace_cache_lock);
}

StackFrame** backtrace_get_current()
{
	HashAndCountInfo hc_info;
	FrameInfo frame_info;
	bool safe_to_get_backtrace = heap_boss_profiler_runtime_is_ready();

	mono_mutex_lock(&backtrace_cache_lock);

	if (backtrace_cache == NULL)
		backtrace_cache = g_hash_table_new_full(NULL, NULL, NULL, backtrace_cache_value_destroy);

	mono_mutex_unlock(&backtrace_cache_lock);

	hc_info.hash = 0;
	hc_info.count = 0;
	if (safe_to_get_backtrace)
		mono_stack_walk_no_il(stack_walk_hash_and_count_fn, &hc_info);

	StackFrame** frame_vec;

	mono_mutex_lock(&backtrace_cache_lock);
	frame_vec = reinterpret_cast<StackFrame**>(g_hash_table_lookup(backtrace_cache, GUINT_TO_POINTER(hc_info.hash)));
	mono_mutex_unlock(&backtrace_cache_lock);

	if (frame_vec != NULL)
		return frame_vec;

	// FIXME: we need to deal with hash collisions

	frame_vec = g_new0(StackFrame*, hc_info.count + 1);
	frame_info.pos = 0;
	frame_info.vec = frame_vec;
	if (safe_to_get_backtrace)
		mono_stack_walk_no_il(stack_walk_build_frame_vector_fn, &frame_info);

	mono_mutex_lock(&backtrace_cache_lock);
	g_hash_table_insert(backtrace_cache, GUINT_TO_POINTER(hc_info.hash), frame_vec);
	mono_mutex_unlock(&backtrace_cache_lock);

	return frame_vec;
}