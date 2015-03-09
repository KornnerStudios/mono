
#ifndef __MONO_PROFILER_PRIVATE_H__
#define __MONO_PROFILER_PRIVATE_H__

#include <mono/metadata/profiler.h>
#include "mono/utils/mono-compiler.h"

extern MonoProfileFlags mono_profiler_events;

enum {
	MONO_PROFILE_START_LOAD,
	MONO_PROFILE_END_LOAD,
	MONO_PROFILE_START_UNLOAD,
	MONO_PROFILE_END_UNLOAD
};

typedef struct {
	int entries;
	struct {
                guchar* cil_code;
                int count;
        } data [1];
} MonoProfileCoverageInfo;

void mono_profiler_shutdown        (void) MONO_INTERNAL;

void mono_profiler_method_enter    (MonoMethod *method) MONO_INTERNAL;
void mono_profiler_method_leave    (MonoMethod *method) MONO_INTERNAL;
void mono_profiler_method_jit      (MonoMethod *method) MONO_INTERNAL;
void mono_profiler_method_end_jit  (MonoMethod *method, MonoJitInfo* jinfo, int result) MONO_INTERNAL;
void mono_profiler_method_free     (MonoMethod *method) MONO_INTERNAL;
void mono_profiler_method_start_invoke (MonoMethod *method) MONO_INTERNAL;
void mono_profiler_method_end_invoke   (MonoMethod *method) MONO_INTERNAL;

void mono_profiler_code_transition (MonoMethod *method, int result) MONO_INTERNAL;
void mono_profiler_allocation      (MonoObject *obj, MonoClass *klass) MONO_INTERNAL;
void mono_profiler_monitor_event   (MonoObject *obj, MonoProfilerMonitorEvent event) MONO_INTERNAL;
void mono_profiler_stat_hit        (guchar *ip, void *context) MONO_INTERNAL;
void mono_profiler_stat_call_chain (int call_chain_depth, guchar **ips, void *context) MONO_INTERNAL;
#define MONO_PROFILER_MAX_STAT_CALL_CHAIN_DEPTH 16
int  mono_profiler_stat_get_call_chain_depth (void) MONO_INTERNAL;
MonoProfilerCallChainStrategy mono_profiler_stat_get_call_chain_strategy (void) MONO_INTERNAL;
void mono_profiler_thread_start    (gsize tid) MONO_INTERNAL;
void mono_profiler_thread_end      (gsize tid) MONO_INTERNAL;

void mono_profiler_exception_thrown         (MonoObject *exception) MONO_INTERNAL;
void mono_profiler_exception_method_leave   (MonoMethod *method) MONO_INTERNAL;
void mono_profiler_exception_clause_handler (MonoMethod *method, int clause_type, int clause_num) MONO_INTERNAL;

void mono_profiler_assembly_event  (MonoAssembly *assembly, int code) MONO_INTERNAL;
void mono_profiler_assembly_loaded (MonoAssembly *assembly, int result) MONO_INTERNAL;

void mono_profiler_module_event  (MonoImage *image, int code) MONO_INTERNAL;
void mono_profiler_module_loaded (MonoImage *image, int result) MONO_INTERNAL;

void mono_profiler_class_event  (MonoClass *klass, int code) MONO_INTERNAL;
void mono_profiler_class_loaded (MonoClass *klass, int result) MONO_INTERNAL;

void mono_profiler_appdomain_event  (MonoDomain *domain, int code) MONO_INTERNAL;
void mono_profiler_appdomain_loaded (MonoDomain *domain, int result) MONO_INTERNAL;

void mono_profiler_iomap (char *report, const char *pathname, const char *new_pathname) MONO_INTERNAL;

MonoProfileCoverageInfo* mono_profiler_coverage_alloc (MonoMethod *method, int entries) MONO_INTERNAL;
void                     mono_profiler_coverage_free  (MonoMethod *method) MONO_INTERNAL;

void mono_profiler_gc_event       (MonoGCEvent e, int generation) MONO_INTERNAL;
void mono_profiler_gc_heap_resize (gint64 new_size) MONO_INTERNAL;
void mono_profiler_gc_moves (void **objects, int num) MONO_INTERNAL;

void mono_profiler_code_chunk_new (gpointer chunk, int size) MONO_INTERNAL;
void mono_profiler_code_chunk_destroy (gpointer chunk) MONO_INTERNAL;
void mono_profiler_code_buffer_new (gpointer buffer, int size, MonoProfilerCodeBufferType type, void *data) MONO_INTERNAL;

void mono_profiler_runtime_initialized (void) MONO_INTERNAL;


// BOSSFIGHT: helper profiler functions
void mono_profiler_gc_boehm_fixed_allocation(gpointer address, size_t size) MONO_INTERNAL;
void mono_profiler_gc_boehm_fixed_free(gpointer address, size_t size) MONO_INTERNAL;
void mono_profiler_gc_boehm_dump_begin() MONO_INTERNAL;
void mono_profiler_gc_boehm_dump_end() MONO_INTERNAL;
void mono_profiler_gc_boehm_dump_heap_section(gpointer start, gpointer end) MONO_INTERNAL;
void mono_profiler_gc_boehm_dump_heap_section_block(gpointer base_address, size_t block_size, size_t object_size, guint8 block_kind, guint8 flags) MONO_INTERNAL;
void mono_profiler_gc_boehm_dump_static_root_set(gpointer start, gpointer end) MONO_INTERNAL;
void mono_profiler_gc_boehm_dump_thread_stack(gint32 thread_id, gpointer stack_start, gpointer stack_end, gpointer registers_start, gpointer registers_end) MONO_INTERNAL;
void mono_profiler_class_vtable_created(MonoDomain* domain, MonoClass* klass, MonoVTable* vtable) MONO_INTERNAL;
// only called on allocations performed in the GC (ie, klass->has_static_refs)
void mono_profiler_class_statics_allocation(MonoDomain* domain, MonoClass* klass, gpointer data, size_t data_size) MONO_INTERNAL;
void mono_profiler_thread_table_allocation(MonoThread** table, size_t table_count, size_t table_size) MONO_INTERNAL;
void mono_profiler_thread_statics_allocation(gpointer data, size_t data_size) MONO_INTERNAL;

#endif /* __MONO_PROFILER_PRIVATE_H__ */

