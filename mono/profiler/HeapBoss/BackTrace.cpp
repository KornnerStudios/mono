
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
}

#include <mono/profiler/HeapBoss/HeapBoss.h>

BackTraceByClass::BackTraceByClass(_MonoClass* klass, const BackTrace* backtrace)
	: klass(klass)
	, owner(backtrace)
{
}

BackTrace::BackTrace(size_t frameCount)
	: frames()
	, klass_backtraces()
{
	frames.reserve(frameCount);
}
BackTrace::~BackTrace()
{
	// delete klass_backtraces values
	for (auto iter = klass_backtraces.begin(), end = klass_backtraces.end();
		iter != end;
		++iter)
	{
		BackTraceByClass* klass_backtrace = iter->second;
		delete klass_backtrace;
	}
}
BackTraceByClass* BackTrace::get_or_add_class(_MonoClass* klass)
{
	BackTraceByClass* klass_bt = NULL;
	auto klass_bt_iter = klass_backtraces.find(klass);
	if (klass_bt_iter != klass_backtraces.end())
	{
		klass_bt = klass_bt_iter->second;
	}
	else
	{
		klass_bt = new BackTraceByClass(klass, this);
		auto kvp = std::pair<_MonoClass*, BackTraceByClass*>(klass, klass_bt);
		klass_backtraces.insert(kvp);
	}

	return klass_bt;
}

struct CollisionCheckInfo
{
	const BackTrace& found_backtrace;
	guint32 frame_index;
	bool collision;

	std::vector<StackFrame> frames;

	CollisionCheckInfo(const BackTrace& backtrace)
		: found_backtrace(backtrace)
		, frame_index(0)
		, collision(false)
		, frames(backtrace.frames.size())
	{
		frames.reserve(backtrace.frames.size());
	}
};
static gboolean stack_walk_check_for_collision_fn(
	MonoMethod* method, gint32 native_offset, gint32 il_offset, gboolean managed, gpointer data)
{
	auto info = reinterpret_cast<CollisionCheckInfo*>(data);
	if (method == NULL)
		return false;

	if (info->frame_index >= info->found_backtrace.frames.size())
	{
		info->collision = true;
		return true;
	}

	const StackFrame& found_frame = info->found_backtrace.frames[info->frame_index];

	if (found_frame.method != method && found_frame.native_offset != native_offset)
	{
		info->collision = true;
		return true;
	}

	StackFrame this_frame;
	this_frame.method = method;
	this_frame.native_offset = native_offset;
	info->frames.push_back(this_frame);

	info->frame_index++;
	return false;
}

struct HashAndCountInfo
{
	gint32 hash;
	guint32 count;
};

static gboolean stack_walk_hash_and_count_fn(
	MonoMethod* method, gint32 native_offset, gint32 il_offset, gboolean managed, gpointer data)
{
	auto info = reinterpret_cast<HashAndCountInfo*>(data);
	if (method == NULL)
		return false;

	const guint32 full_c = 2909;
	const guint32 method_c = 277;
	const guint32 native_c = 163;
	// stack is walked with no IL, offset will be -1
//	const guint32 il_c = 47;

	// TODO: 64-bit this
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
	std::vector<StackFrame> frames;
};

static gboolean stack_walk_build_frame_vector_fn(
	MonoMethod* method, gint32 native_offset, gint32 il_offset, gboolean managed, gpointer data)
{
	if (method == NULL)
		return false;

	auto bt = reinterpret_cast<BackTrace*>(data);

	StackFrame frame;
	frame.method = method;
	frame.native_offset = native_offset;
	bt->frames.push_back(frame);

	return false;
}

static void backtrace_cache_value_destroy(gpointer data)
{
	auto backtrace = reinterpret_cast<BackTrace*>(data);

	delete backtrace;
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

size_t backtrace_misses;

BackTraceByClass*
backtrace_get_current(MonoObject* obj, MonoClass* klass)
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
	else
		backtrace_misses++;


	mono_mutex_lock(&backtrace_cache_lock);
	auto* backtrace = reinterpret_cast<BackTrace*>(g_hash_table_lookup(backtrace_cache, GUINT_TO_POINTER(hc_info.hash)));

	mono_mutex_unlock(&backtrace_cache_lock);

	if (backtrace != NULL)
	{
		BackTraceByClass* klass_backtrace = backtrace->get_or_add_class(klass);
#if _DEBUG
		if (safe_to_get_backtrace)
		{
			CollisionCheckInfo collision_check(*backtrace);

			mono_stack_walk_no_il(stack_walk_check_for_collision_fn, &collision_check);

			if (collision_check.collision)
			{
				collision_check.frames.shrink_to_fit();

				MonoType* type = mono_class_get_type(klass);
				char* type_name = mono_type_full_name(type);

				DebugBreak();
				g_free(type_name);
			}
			/*else if (backtrace->klass != klass)
			{
				MonoType* type = mono_class_get_type(klass);
				char* type_name = mono_type_full_name(type);

				MonoType* bt_type = mono_class_get_type(backtrace->klass);
				char* bt_type_name = mono_type_full_name(bt_type);

				DebugBreak();
				g_free(type_name);
				g_free(bt_type_name);
			}*/
		}
#endif

		return klass_backtrace;
	}

	// FIXME: we need to deal with hash collisions

	backtrace = new BackTrace(hc_info.count + 1);
	if (safe_to_get_backtrace)
		mono_stack_walk_no_il(stack_walk_build_frame_vector_fn, backtrace);

	backtrace->frames.shrink_to_fit();

	mono_mutex_lock(&backtrace_cache_lock);
	g_hash_table_insert(backtrace_cache, GUINT_TO_POINTER(hc_info.hash), backtrace);
	BackTraceByClass* klass_backtrace = backtrace->get_or_add_class(klass);
	mono_mutex_unlock(&backtrace_cache_lock);

	return klass_backtrace;
}