#pragma once

#if defined(_MSC_VER)
#include <Windows.h>
#endif

#define HEAP_BOSS_TRACK_INDIVIDUAL_OBJECTS 1

struct Accountant;
struct ios_app_memory_stats;

int gettimeofday(struct timeval * tp, struct timezone * tzp);

struct OutfileWriter
{
	struct total_data
	{
		uint32_t gc_count;
		uint32_t type_count;
		uint32_t method_count;
		uint32_t backtrace_count;
		uint32_t resize_count;

		uint64_t frames_count;
		uint64_t object_news_count, object_resizes_count, object_gcs_count;
		uint64_t boehm_news_count, boehm_frees_count;
	};

	FILE* out;
	gboolean auto_flush;
	GHashTable* seen_items;
	total_data total;
	fpos_t saved_outfile_offset;
	time_t saved_outfile_timestamp;
	uint64_t saved_outfile_nanos_start;

	fpos_t heap_memory_header_offset;
	uint32_t heap_sections_written_count;

	fpos_t heap_section_header_offset;
	uint32_t heap_section_blocks_written_count;

	uint32_t heap_memory_total_heap_bytes;
	uint32_t heap_memory_total_bytes_written;
	uint32_t heap_memory_total_roots;
	uint32_t heap_memory_total_threads;

	~OutfileWriter();

	uint64_t get_nanoseconds_offset() const;
	uint64_t get_timestamp_offset() const;

	void try_flush();

	void write_class_if_not_already_seen(MonoClass* klass);

	// written after every GC and on file close
	void write_counts(guint64 total_allocated_bytes, guint32 total_allocated_objects, bool write_totals);

	void write_heap_stats(uint64_t heap_size, uint64_t heap_used_size);
	void write_ios_memory_stats(ios_app_memory_stats* stats);

	void write_object_new(const MonoClass* klass, const MonoObject* obj, uint32_t size, gpointer backtrace);
	void write_object_resize(const MonoClass* klass, gpointer backtrace, const MonoObject* obj, uint32_t new_size);
	void write_object_gc(const MonoClass* klass, gpointer backtrace, const MonoObject* obj);

	void write_custom_event(const char* event_string);

	void write_app_resign_active();
	void write_app_become_active();

	void write_new_frame();

	void write_heap();
	void write_heap_end();
	void write_heap_section(const void* start, const void* end);
	void write_heap_section_block(const void* start, size_t block_size, size_t obj_size, uint8_t block_kind, bool is_free);
	void write_heap_section_end();
	void write_heap_root_sets_start();
	void write_heap_root_set(const void* start, const void* end);
	void write_thread_stack(int32_t thread_id, const void* stack, size_t stack_size, const void* registers, size_t registers_size);

	void write_boehm_allocation(gpointer address, size_t size);
	void write_boehm_free(gpointer address, size_t size);

	void write_class_vtable_created(MonoDomain* domain, MonoClass* klass, MonoVTable* vtable);
	void write_class_statics_allocation(MonoDomain* domain, MonoClass* klass, gpointer data, size_t data_size);

	void write_thread_table_allocation(MonoThread** table, size_t table_count, size_t table_size);
	void write_thread_statics_allocation(gpointer data, size_t data_size);
};

OutfileWriter* outfile_writer_open(const char* filename);

void           outfile_writer_close(OutfileWriter* ofw);

void           outfile_writer_add_accountant(OutfileWriter* ofw, Accountant* acct);

void           outfile_writer_gc_begin(OutfileWriter* ofw,
									   gboolean       is_final,
									   guint64        total_live_bytes,
									   guint32        total_live_objects,
									   guint32        n_accountants);

void           outfile_writer_gc_log_stats(OutfileWriter* ofw, Accountant* acct);

void           outfile_writer_gc_end(OutfileWriter* ofw,
									 guint64        total_allocated_bytes,
									 guint32        total_allocated_objects,
									 guint64        total_live_bytes,
									 guint32        total_live_objects);

void           outfile_writer_resize(OutfileWriter* ofw,
									 guint64        new_size,
									 guint64        total_live_bytes,
									 guint32        total_live_objects);
