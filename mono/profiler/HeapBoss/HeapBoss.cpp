
#include <mono/profiler/HeapBoss/HeapBoss.h>
#include <mono/profiler/HeapBoss/Stacktrace.hpp>

#ifdef PLATFORM_WIN32 // because fucking mono-mutex.h doesn't include this shit
#include <windows.h>
#endif

#include <string.h>
extern "C"
{
#include <glib.h>
#include <mono/metadata/assembly.h>
#include <mono/io-layer/mono-mutex.h>
#include <mono/metadata/class.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/object.h>
#include <mono/metadata/profiler.h>
#include <mono/metadata/mono-gc.h>

#include <gc.h>
_MonoThread* mono_thread_current();
}

#if PLATFORM_WIN32 // mkdir includes
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include <mono/profiler/HeapBoss/Accountant.hpp>
#include <mono/profiler/HeapBoss/BackTrace.hpp>
#include <mono/profiler/HeapBoss/OutFileWriter.hpp>

struct _MonoProfiler
{
private:
	static const bool cIgnoreDumpNextHeapCond = false;
public:

	bool runtime_is_ready;
	bool gc_heap_dumping_enabled;
	bool gc_stacktrace_dumping_enabled;
	bool should_dump_next_heap;

	mono_mutex_t   lock;
	GHashTable*    accountant_hash;
	GHashTable*    stacktrace_hash;
	guint64        total_allocated_bytes;
	guint64        total_live_bytes;
	guint32        total_allocated_objects;
	guint32        total_live_objects;
	guint32        n_dirty_accountants;
	OutfileWriter* outfile_writer;

	bool should_dump_boehm_heap() const
	{
		return gc_heap_dumping_enabled && (cIgnoreDumpNextHeapCond || should_dump_next_heap);
	}

	static void accountant_hash_value_destroy(gpointer data)
	{
		auto acct = reinterpret_cast<Accountant*>(data);

		delete acct;
	}

	static void stacktrace_hash_value_destroy(gpointer data)
	{
		auto str = reinterpret_cast<char*>(data);

		free(str);
	}
};

MonoClass* get_mono_thread_class();
void write_missed_vtables_hack(OutfileWriter* outfile_writer);
static gpointer g_heap_boss_gc_boehm_alloc_last_addr;
void heap_boss_gc_boehm_alloc(GC_PTR ptr, size_t size);
void heap_boss_gc_boehm_free(GC_PTR ptr, size_t size_hint);

static MonoProfiler* g_heap_boss_profiler;

static MonoProfiler* create_mono_profiler(
	const char *outfilename)
{
	if (g_heap_boss_profiler != NULL)
		return g_heap_boss_profiler;

	bossfight_mono_set_backtrace_callback(GetStacktrace);

	MonoProfiler* p = g_new0(MonoProfiler, 1);

	mono_mutex_init(&p->lock, NULL);

	backtrace_cache_initialize();

	p->gc_stacktrace_dumping_enabled = true;

	p->accountant_hash = g_hash_table_new_full(NULL, NULL, NULL, MonoProfiler::accountant_hash_value_destroy);
	p->stacktrace_hash = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, MonoProfiler::stacktrace_hash_value_destroy);
	p->total_live_bytes = 0;
	p->outfile_writer = outfile_writer_open(outfilename);

	g_heap_boss_profiler = p;

	GC_on_malloc_callback = heap_boss_gc_boehm_alloc;
	GC_on_free_callback = heap_boss_gc_boehm_free;

	return p;
}

extern size_t backtrace_misses;
static void dispose_mono_profiler(MonoProfiler* p)
{
	write_missed_vtables_hack(p->outfile_writer);

	outfile_writer_close(p->outfile_writer);
	p->outfile_writer = NULL;

	g_hash_table_destroy(p->accountant_hash);
	p->accountant_hash = NULL;

	g_hash_table_destroy(p->stacktrace_hash);
	p->stacktrace_hash = NULL;

	backtrace_cache_dispose();

	mono_mutex_destroy(&p->lock);

	if (g_heap_boss_profiler == p)
		g_heap_boss_profiler = NULL;

	if (backtrace_misses)
		backtrace_misses = 0;

	g_free(p);
}

/* ** ** ** ** ** ** ** ** ** ** ** ** ** ** ** ** ** ** ** ** ** ** ** ** */

static void heap_boss_profiler_runtime_initialized(MonoProfiler* p)
{
	if (g_heap_boss_profiler == NULL)
		return;

	mono_mutex_lock(&p->lock);
	p->runtime_is_ready = true;
	mono_mutex_unlock(&p->lock);
}

static void heap_boss_alloc_func(MonoProfiler* p, MonoObject* obj, MonoClass* klass)
{
	if (g_heap_boss_profiler == NULL || !p->runtime_is_ready)
		return;

	//StackFrame** backtrace = backtrace_get_current(obj, klass);
	//BackTrace* backtrace = backtrace_get_current(obj, klass);
	BackTraceByClass* backtrace = backtrace_get_current(obj, klass);

	mono_mutex_lock(&p->lock);
#if 0
	//if (reinterpret_cast<uintptr_t>(obj)& 0xCF0)
	if (klass == get_mono_thread_class())
	{
		gpointer ptr = g_heap_boss_gc_boehm_alloc_last_addr;
		MessageBoxA(NULL, "alloc_func", "", MB_OK);
	}
#endif

	auto acct = reinterpret_cast<Accountant*>(g_hash_table_lookup(p->accountant_hash, backtrace));
	if (acct == NULL)
	{
		acct = accountant_new(klass, backtrace);
		g_hash_table_insert(p->accountant_hash, backtrace, acct);
		outfile_writer_add_accountant(p->outfile_writer, acct);
	}

	guint size = mono_object_get_size(obj);
	acct->register_object(obj, size);
	p->total_allocated_bytes += size;
	p->total_live_bytes += size;
	p->total_allocated_objects++;
	p->total_live_objects++;

#if HEAP_BOSS_TRACK_INDIVIDUAL_OBJECTS
	p->outfile_writer->write_object_new(acct->klass, obj, size, acct->backtrace);
#endif

	mono_mutex_unlock(&p->lock);
}

static void post_gc_tallying_fn(gpointer key, gpointer value, gpointer user_data)
{
	auto p = reinterpret_cast<MonoProfiler*>(user_data);
	auto acct = reinterpret_cast<Accountant*>(value);

	acct->post_gc_processing(p->outfile_writer);
	p->total_live_bytes += acct->n_live_bytes;
	p->total_live_objects += acct->n_live_objects;
	if (acct->dirty)
		++p->n_dirty_accountants;
}

static void post_gc_logging_fn(gpointer key, gpointer value, gpointer user_data)
{
	auto p = reinterpret_cast<MonoProfiler*>(user_data);
	auto acct = reinterpret_cast<Accountant*>(value);

	// Only log the accountant's stats to the outfile if
	// something has changed since the last time it was logged.
	if (acct->dirty)
	{
		outfile_writer_gc_log_stats(p->outfile_writer, acct);
		acct->dirty = FALSE;
	}
}

static void heap_boss_gc_func(MonoProfiler* p, MonoGCEvent e, int gen)
{
	if (g_heap_boss_profiler == NULL)
		return;

	guint64 prev_total_live_bytes;
	guint32 prev_total_live_objects;

	if (e != MONO_GC_EVENT_MARK_END)
		return;

	mono_mutex_lock(&p->lock);

	prev_total_live_bytes = p->total_live_bytes;
	prev_total_live_objects = p->total_live_objects;

	p->total_live_bytes = 0;
	p->total_live_objects = 0;
	p->n_dirty_accountants = 0;
	g_hash_table_foreach(p->accountant_hash, post_gc_tallying_fn, p);

	outfile_writer_gc_begin(p->outfile_writer,
							gen < 0, // negative gen == this is final
							prev_total_live_bytes,
							prev_total_live_objects,
							p->n_dirty_accountants);
	g_hash_table_foreach(p->accountant_hash, post_gc_logging_fn, p);
	outfile_writer_gc_end(p->outfile_writer,
						  p->total_allocated_bytes,
						  p->total_allocated_objects,
						  p->total_live_bytes,
						  p->total_live_objects);

	p->outfile_writer->write_heap_stats(mono_gc_get_heap_size(), mono_gc_get_used_size());

#if FALSE // iOS
	ios_app_memory_stats ios_mem_stats;
	if (ios_app_get_memory(&ios_mem_stats))
		p->outfile_writer->write_ios_memory_stats(&ios_mem_stats);
#endif

	mono_mutex_unlock(&p->lock);
}

static void heap_boss_gc_resize_func(MonoProfiler* p, gint64 new_size)
{
	if (g_heap_boss_profiler == NULL)
		return;

	mono_mutex_lock(&p->lock);

	outfile_writer_resize(p->outfile_writer,
						  new_size,
						  p->total_live_bytes,
						  p->total_live_objects);

	p->outfile_writer->write_heap_stats(mono_gc_get_heap_size(), mono_gc_get_used_size());

#if FALSE // iOS
	ios_app_memory_stats ios_mem_stats;
	if (ios_app_get_memory(&ios_mem_stats))
		p->outfile_writer->write_ios_memory_stats(&ios_mem_stats);
#endif

	mono_mutex_unlock(&p->lock);
}

static void heap_boss_gc_boehm_alloc(GC_PTR ptr, size_t size)
{
	if (g_heap_boss_profiler == NULL)
		return;

	mono_mutex_lock(&g_heap_boss_profiler->lock);
	g_heap_boss_gc_boehm_alloc_last_addr = ptr;

	uint32_t stacktrace_hash = 0;
	if (g_heap_boss_profiler->gc_stacktrace_dumping_enabled && gGetStacktraceForBossFight != NULL)
	{
		char stacktrace_buffer[2048] = "";
		gGetStacktraceForBossFight(stacktrace_buffer, sizeof(stacktrace_buffer), 12); // TODO: expose max frames as setting?
		stacktrace_hash = g_str_hash(stacktrace_buffer);

		auto str = reinterpret_cast<char*>(g_hash_table_lookup(g_heap_boss_profiler->stacktrace_hash, stacktrace_buffer));
		if (str == NULL)
		{
			str = _strdup(stacktrace_buffer);
			g_hash_table_insert(g_heap_boss_profiler->stacktrace_hash, str, str);
			g_heap_boss_profiler->outfile_writer->write_boehm_allocation_stacktrace(stacktrace_hash, str);
		}
	}

	g_heap_boss_profiler->outfile_writer->write_boehm_allocation(ptr, size, stacktrace_hash);
	mono_mutex_unlock(&g_heap_boss_profiler->lock);
}
static void heap_boss_gc_boehm_free(GC_PTR ptr, size_t size_hint)
{
	if (g_heap_boss_profiler == NULL)
		return;

	mono_mutex_lock(&g_heap_boss_profiler->lock);
	g_heap_boss_profiler->outfile_writer->write_boehm_free(ptr, size_hint);
	mono_mutex_unlock(&g_heap_boss_profiler->lock);
}

static void heap_boss_gc_boehm_fixed_alloc(MonoProfiler* p, gpointer address, size_t size)
{
	if (g_heap_boss_profiler == NULL)
		return;

	mono_mutex_lock(&p->lock);
	p->outfile_writer->write_boehm_allocation(address, size);
	mono_mutex_unlock(&p->lock);
}
static void heap_boss_gc_boehm_fixed_free(MonoProfiler* p, gpointer address, size_t size)
{
	if (g_heap_boss_profiler == NULL)
		return;

	mono_mutex_lock(&p->lock);
	p->outfile_writer->write_boehm_free(address, size);
	mono_mutex_unlock(&p->lock);
}

static void heap_boss_gc_boehm_dump_begin(MonoProfiler* p)
{
	if (g_heap_boss_profiler == NULL)
		return;

	mono_mutex_lock(&p->lock);
	if (p->should_dump_boehm_heap())
		p->outfile_writer->write_heap();
	mono_mutex_unlock(&p->lock);
}
static void heap_boss_gc_boehm_dump_end(MonoProfiler* p)
{
	if (g_heap_boss_profiler == NULL)
		return;

	mono_mutex_lock(&p->lock);
	if (p->should_dump_boehm_heap())
	{
		p->outfile_writer->write_heap_end();

		p->should_dump_next_heap = false;
	}
	mono_mutex_unlock(&p->lock);
}
static void heap_boss_gc_boehm_dump_heap_section(MonoProfiler* p, gpointer start, gpointer end)
{
	if (g_heap_boss_profiler == NULL)
		return;

	mono_mutex_lock(&p->lock);
	if (p->should_dump_boehm_heap())
	{
		const gpointer cEndSignal = reinterpret_cast<gpointer>(-1);

		if (start == cEndSignal && end == cEndSignal)
			p->outfile_writer->write_heap_section_end();
		else
		{
			p->outfile_writer->write_heap_section(start, end);
		}
	}
	mono_mutex_unlock(&p->lock);
}
static void heap_boss_gc_boehm_dump_heap_section_block(MonoProfiler* p, 
	gpointer base_address, size_t block_size, size_t object_size, guint8 block_kind, guint8 flags)
{
	if (g_heap_boss_profiler == NULL)
		return;

	mono_mutex_lock(&p->lock);
	bool is_free = flags & 0x01;
	if (p->should_dump_boehm_heap())
		p->outfile_writer->write_heap_section_block(base_address, block_size, object_size, block_kind, is_free);
	mono_mutex_unlock(&p->lock);
}
static void heap_boss_gc_boehm_dump_root_set(MonoProfiler* p, gpointer start, gpointer end)
{
	if (g_heap_boss_profiler == NULL)
		return;

	mono_mutex_lock(&p->lock);
	if (p->should_dump_boehm_heap())
		p->outfile_writer->write_heap_root_set(start, end);
	mono_mutex_unlock(&p->lock);
}
static void heap_boss_gc_boehm_dump_thread_stack(MonoProfiler* p,
	gint32 thread_id, gpointer stack_start, gpointer stack_end, gpointer registers_start, gpointer registers_end)
{
	if (g_heap_boss_profiler == NULL)
		return;

	mono_mutex_lock(&p->lock);
	if (p->should_dump_boehm_heap())
	{
		size_t stack_size = (uintptr_t)stack_end - (uintptr_t)stack_start;
		size_t regs_size = (uintptr_t)registers_end - (uintptr_t)registers_start;
		p->outfile_writer->write_thread_stack(thread_id, stack_start, stack_size, registers_start, regs_size);
	}
	mono_mutex_unlock(&p->lock);
}

static void heap_boss_class_vtable_created(MonoProfiler* p,
	MonoDomain* domain, MonoClass* klass, MonoVTable* vtable)
{
	if (g_heap_boss_profiler == NULL)
		return;

	mono_mutex_lock(&p->lock);
	p->outfile_writer->write_class_vtable_created(domain, klass, vtable);
	mono_mutex_unlock(&p->lock);
}

static void heap_boss_class_statics_allocation(MonoProfiler* p,
	MonoDomain* domain, MonoClass* klass, gpointer data, size_t data_size)
{
	if (g_heap_boss_profiler == NULL)
		return;

	mono_mutex_lock(&p->lock);
	p->outfile_writer->write_class_statics_allocation(domain, klass, data, data_size);
	mono_mutex_unlock(&p->lock);
}

static void heap_boss_thread_table_allocation(MonoProfiler* p,
	MonoThread** table, size_t table_count, size_t table_size)
{
	if (g_heap_boss_profiler == NULL)
		return;

	mono_mutex_lock(&p->lock);
	p->outfile_writer->write_thread_table_allocation(table, table_count, table_size);
	mono_mutex_unlock(&p->lock);
}
static void heap_boss_thread_statics_allocation(MonoProfiler* p,
	gpointer data, size_t data_size)
{
	if (g_heap_boss_profiler == NULL)
		return;

	mono_mutex_lock(&p->lock);
	p->outfile_writer->write_thread_statics_allocation(data, data_size);
	mono_mutex_unlock(&p->lock);
}

static void heap_boss_shutdown(MonoProfiler* p)
{
	if (g_heap_boss_profiler == NULL)
		return;

	// Do a final, synthetic GC
	heap_boss_gc_func(p, MONO_GC_EVENT_MARK_END, -1);

	dispose_mono_profiler(p);

	printf("heap-boss shutdown");
}

static tm* get_current_tm()
{
#if PLATFORM_WIN32
	time_t rawtime;
	time(&rawtime);

	return localtime(&rawtime);
#else
	timeval tv;
	gettimeofday(&tv, NULL);

	return localtime(&tv.tv_sec);
#endif
}

extern "C"
void heap_boss_startup(const char* desc)
{
#if PLATFORM_WIN32
	MessageBoxA(NULL, "Attach debugger now", "HeapBoss", MB_OK);
#endif

	MonoProfiler* p;

	char outfile[FILENAME_MAX] = "\0";
	const char* outfilename;

	g_assert(!strncmp(desc, "heap-boss", 9));

	outfilename = strchr(desc, ':');
	if (outfilename == NULL)
	{
		char tv_string[64] = "no-date";
		tm* tm;

		if ((tm = get_current_tm()) != NULL)
		{
			strftime(tv_string, sizeof tv_string, "%Y-%m-%d_%H;%M;%S", tm);
		}

#ifdef PLATFORM_WIN32
		const char* outfiles_dir = "outfiles\\";
		_mkdir(outfiles_dir);
#else
		const char* outfiles_dir = "outfiles/";
		mkdir(outfiles_dir, 0777);
#endif

		strcpy(outfile, outfiles_dir);
		strcat(outfile, "outfile_");
		strcat(outfile, tv_string);
	}
	else
	{
		// Advance past the : and use the rest as the name.
		++outfilename;
	}

	printf("*** Init heap-boss ***\n");

	p = create_mono_profiler(outfile);
	mono_profiler_install(p, heap_boss_shutdown);

	// HACK: hard code this to true if we're not installing before mini_init finishes
	p->runtime_is_ready = true;
	mono_profiler_install_runtime_initialized(heap_boss_profiler_runtime_initialized);

	mono_profiler_install_allocation(heap_boss_alloc_func);

	mono_profiler_install_gc(heap_boss_gc_func, heap_boss_gc_resize_func);

	mono_profiler_install_gc_boehm(heap_boss_gc_boehm_fixed_alloc, heap_boss_gc_boehm_fixed_free);
	mono_profiler_install_gc_boehm_dump(heap_boss_gc_boehm_dump_begin, heap_boss_gc_boehm_dump_end,
		heap_boss_gc_boehm_dump_heap_section, heap_boss_gc_boehm_dump_heap_section_block, 
		heap_boss_gc_boehm_dump_root_set, heap_boss_gc_boehm_dump_thread_stack);

	mono_profiler_install_class_vtable_created(heap_boss_class_vtable_created);
	mono_profiler_install_class_statics_allocation(heap_boss_class_statics_allocation);

	mono_profiler_install_thread_table_allocation(heap_boss_thread_table_allocation);
	mono_profiler_install_thread_statics_allocation(heap_boss_thread_statics_allocation);

	const auto profile_flags = static_cast<MonoProfileFlags>(
		MONO_PROFILE_ALLOCATIONS | MONO_PROFILE_GC | 
		MONO_PROFILER_GC_BOEHM_EVENTS | MONO_PROFILER_GC_BOEHM_DUMP_EVENTS);
	mono_profiler_set_events(profile_flags);

	// route around Unity's bullshit https://twitter.com/KornnerStudios/status/574729839250272257
	mono_profiler_install(NULL, NULL);

	printf("*** Running with heap-boss ***\n");
}

extern "C"
gboolean heap_boss_profiler_runtime_is_ready()
{
	return g_heap_boss_profiler != NULL 
		&& mono_thread_current() != NULL
		//&& g_heap_boss_profiler->runtime_is_ready
		;
}

extern "C"
void heap_boss_handle_custom_event(const char* text)
{
	if (g_heap_boss_profiler == NULL || text == NULL)
		return;

	//printf("boss custom event: %s\n\n", text);

	bool gc_heap_dumping_enabled = 0 == strcmp(text, "tap_to_play_shown");

	mono_mutex_lock(&g_heap_boss_profiler->lock);
	if (!g_heap_boss_profiler->gc_heap_dumping_enabled)
	{
		g_heap_boss_profiler->gc_heap_dumping_enabled = gc_heap_dumping_enabled;
		g_heap_boss_profiler->should_dump_next_heap = true;
	}
	else
	{
		g_heap_boss_profiler->should_dump_next_heap = 
			true // this will force it to only dump the heap after a custom event is signaled
			//0==strcmp(text, "")
			;
	}

	g_heap_boss_profiler->outfile_writer->write_custom_event(text);
	mono_mutex_unlock(&g_heap_boss_profiler->lock);
}

void heap_boss_handle_app_resign_active()
{
	if (g_heap_boss_profiler == NULL)
		return;

	mono_mutex_lock(&g_heap_boss_profiler->lock);
	//g_heap_boss_profiler->gc_heap_dumping_enabled = false;
	g_heap_boss_profiler->outfile_writer->write_app_resign_active();
	mono_mutex_unlock(&g_heap_boss_profiler->lock);
}
void heap_boss_handle_app_become_active()
{
	if (g_heap_boss_profiler == NULL)
		return;

	mono_mutex_lock(&g_heap_boss_profiler->lock);
	g_heap_boss_profiler->outfile_writer->write_app_become_active();
	mono_mutex_unlock(&g_heap_boss_profiler->lock);
}
void heap_boss_handle_app_memory_warning()
{
	if (g_heap_boss_profiler == NULL)
		return;

	mono_mutex_lock(&g_heap_boss_profiler->lock);
	g_heap_boss_profiler->outfile_writer->write_custom_event("mem_warning");
	mono_mutex_unlock(&g_heap_boss_profiler->lock);
}
void heap_boss_handle_app_will_terminate()
{
	if (g_heap_boss_profiler == NULL)
		return;

	mono_mutex_lock(&g_heap_boss_profiler->lock);
	g_heap_boss_profiler->gc_heap_dumping_enabled = false;
	g_heap_boss_profiler->outfile_writer->write_custom_event("terminate");
	mono_mutex_unlock(&g_heap_boss_profiler->lock);
}

void heap_boss_handle_new_frame()
{
	if (g_heap_boss_profiler == NULL)
		return;

	mono_mutex_lock(&g_heap_boss_profiler->lock);
	g_heap_boss_profiler->outfile_writer->write_new_frame();
	mono_mutex_unlock(&g_heap_boss_profiler->lock);
}

// stolen from class-internals.h
typedef struct {
	MonoImage *corlib;
	MonoClass *object_class;
	MonoClass *byte_class;
	MonoClass *void_class;
	MonoClass *boolean_class;
	MonoClass *sbyte_class;
	MonoClass *int16_class;
	MonoClass *uint16_class;
	MonoClass *int32_class;
	MonoClass *uint32_class;
	MonoClass *int_class;
	MonoClass *uint_class;
	MonoClass *int64_class;
	MonoClass *uint64_class;
	MonoClass *single_class;
	MonoClass *double_class;
	MonoClass *char_class;
	MonoClass *string_class;
	MonoClass *enum_class;
	MonoClass *array_class;
	MonoClass *delegate_class;
	MonoClass *multicastdelegate_class;
	MonoClass *asyncresult_class;
	MonoClass *manualresetevent_class;
	MonoClass *typehandle_class;
	MonoClass *fieldhandle_class;
	MonoClass *methodhandle_class;
	MonoClass *systemtype_class;
	MonoClass *monotype_class;
	MonoClass *exception_class;
	MonoClass *threadabortexception_class;
	MonoClass *thread_class;
	MonoClass *transparent_proxy_class;
	MonoClass *real_proxy_class;
	MonoClass *mono_method_message_class;
	MonoClass *appdomain_class;
	MonoClass *field_info_class;
	MonoClass *method_info_class;
	MonoClass *stringbuilder_class;
	MonoClass *math_class;
	MonoClass *stack_frame_class;
	MonoClass *stack_trace_class;
	MonoClass *marshal_class;
	MonoClass *iserializeable_class;
	MonoClass *serializationinfo_class;
	MonoClass *streamingcontext_class;
	MonoClass *typed_reference_class;
	MonoClass *argumenthandle_class;
	MonoClass *marshalbyrefobject_class;
	MonoClass *monitor_class;
	MonoClass *iremotingtypeinfo_class;
	MonoClass *runtimesecurityframe_class;
	MonoClass *executioncontext_class;
	MonoClass *internals_visible_class;
	MonoClass *generic_ilist_class;
	MonoClass *generic_nullable_class;
	MonoClass *variant_class;
	MonoClass *com_object_class;
	MonoClass *com_interop_proxy_class;
	MonoClass *iunknown_class;
	MonoClass *idispatch_class;
	MonoClass *safehandle_class;
	MonoClass *handleref_class;
	MonoClass *attribute_class;
	MonoClass *customattribute_data_class;
	MonoClass *critical_finalizer_object;
} MonoDefaults;
extern "C" MonoDefaults mono_defaults;

static MonoClass* get_mono_thread_class() { return mono_defaults.thread_class; }

static void write_missed_vtables_hack(OutfileWriter* outfile_writer)
{
	MonoDomain* domain = mono_domain_get();
	MonoClass* klass;
	MonoVTable* vtable;

	// write System.MonoType vtable
	klass = mono_defaults.monotype_class;
	vtable = mono_class_vtable(domain, klass);
	if (vtable != NULL)
		outfile_writer->write_class_vtable_created(domain, klass, vtable);

	// write string vtable
	klass = mono_defaults.string_class;
	vtable = mono_class_vtable(domain, klass);
	if (vtable != NULL)
		outfile_writer->write_class_vtable_created(domain, klass, vtable);

	// write object vtable
	klass = mono_defaults.object_class;
	vtable = mono_class_vtable(domain, klass);
	if (vtable != NULL)
		outfile_writer->write_class_vtable_created(domain, klass, vtable);

	// write System.Threading.Thread vtable
	klass = mono_defaults.thread_class;
	vtable = mono_class_vtable(domain, klass);
	if (vtable != NULL)
		outfile_writer->write_class_vtable_created(domain, klass, vtable);
}