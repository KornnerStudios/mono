
#include <mono/profiler/HeapBoss/OutFileWriter.hpp>

#include <string.h>
#include <assert.h>

extern "C" {
#include <glib.h>
#include <mono/metadata/debug-helpers.h>
#include <mono/metadata/mono-debug.h>
#include <unity/unity_utils.h>
}

#include <mono/profiler/HeapBoss/OutFileWriterPriv.inl>
#include <mono/profiler/HeapBoss/Accountant.hpp>
#include <mono/profiler/HeapBoss/BackTrace.hpp>

OutfileWriter* outfile_writer_open(
	const char* filename)
{
	auto timestamp = get_ms_since_epoch();
	uint64_t nanoseconds = get_nanoseconds();

	auto ofw = new OutfileWriter();
	memset(&ofw->total, 0, sizeof(ofw->total));

#if FALSE
	ofw->out = iosfopen(filename, "wb+");
#elif WIN32
	ofw->out = unity_fopen(filename, "wb+");
#else
	ofw->out = fopen(filename, "wb+");
#endif
	assert(ofw->out);

	ofw->seen_items = g_hash_table_new(NULL, NULL);

	write_uint32(ofw->out, cFileSignature);
	write_int32(ofw->out, cFileVersion);
	write_string(ofw->out, cFileLabel);
	write_time(ofw->out, timestamp);
	write_byte(ofw->out, sizeof(gpointer));
	write_byte(ofw->out, 0); // TODO: platform id

	fgetpos(ofw->out, &ofw->saved_outfile_offset);
	ofw->saved_outfile_timestamp = timestamp;
	ofw->saved_outfile_nanos_start = nanoseconds;

	ofw->auto_flush = true;

	// we update these after every GC
	write_byte(ofw->out, 0);   // is the log fully written out?
	ofw->write_counts(0, 0, true);

	return ofw;
}

void outfile_writer_close(
	OutfileWriter* ofw)
{
	auto timestamp = get_ms_since_epoch();

	// Write out the end-of-stream tag.
	write_byte(ofw->out, cTagEos);
	write_time(ofw->out, timestamp);

	// Seek back up to the right place in the header
	fsetpos(ofw->out, &ofw->saved_outfile_offset);

	// Indicate that we terminated normally.
	write_byte(ofw->out, 1);
	ofw->write_counts(0, 0, false);

	fclose(ofw->out);

	delete ofw;
}

static void outfile_writer_update_totals(
	OutfileWriter* ofw,
	gint64         total_allocated_bytes,
	gint32         total_allocated_objects,
	bool write_allocated_totals)
{
	fpos_t old_fpos;
	fgetpos(ofw->out, &old_fpos);

	// Seek back up to the right place in the header
	fsetpos(ofw->out, &ofw->saved_outfile_offset);

	// Update our counts and totals
	write_byte(ofw->out, 0); // we still might terminate abnormally
	ofw->write_counts(total_allocated_bytes, total_allocated_objects, write_allocated_totals);

	// Seek back to the end of the outfile
	fsetpos(ofw->out, &old_fpos);

	ofw->try_flush();
}

static void write_mono_class(
	FILE* file,
	MonoClass* klass)
{
	enum {
		cIsValueTypeBit,
		cHasReferencesBit,
		cHasStaticReferencesBit,
	};
	guint8 flags = 0;

	MonoType* type = mono_class_get_type(klass);

	bool is_value_type = 0 != mono_class_is_valuetype(klass);
	gint32 size_of = is_value_type
		? mono_class_value_size(klass, NULL)
		: mono_class_instance_size(klass);
	gint32 min_alignment = mono_class_min_align(klass);

	if (is_value_type)
		flags |= 1 << cIsValueTypeBit;
	if (mono_bf_class_has_references(klass))
		flags |= 1 << cHasReferencesBit;
	if (mono_bf_class_has_static_references(klass))
		flags |= 1 << cHasStaticReferencesBit;

	write_byte(file, cTagType);
	write_pointer(file, klass);

	char* name = mono_type_full_name(type);
	write_string(file, name);
	g_free(name);

	write_byte(file, flags);
	write_int32(file, size_of);
	write_int32(file, min_alignment);
}

static guint32 g_backtrace_frame_debug_lines[512];
static size_t write_backtrace_methods(
	FILE* file,
	GHashTable* seen_methods,
	StackFrame** backtrace,
	uint32_t& total_unique_methods)
{
	size_t frame_count = 0;
	MonoDomain* domain = mono_domain_get();
	for (size_t i = 0; backtrace[i] != NULL; ++i, ++frame_count)
	{
		g_assert(i < _countof(g_backtrace_frame_debug_lines));
		auto* frame = backtrace[i];

		MonoMethod* method = frame->method;
		MonoDebugSourceLocation* debug_loc = mono_debug_lookup_source_location(method, frame->native_offset, domain);

		if (g_hash_table_lookup(seen_methods, method) == NULL)
		{
			MonoClass* klass = mono_method_get_class(method);

			write_byte(file, cTagMethod);
			write_pointer(file, method);
			write_pointer(file, klass);

			char *name = mono_method_full_name(method, TRUE);
			write_string(file, name);
			g_free(name);

			write_string(file, debug_loc != NULL ? debug_loc->source_file : "");

			g_hash_table_insert(seen_methods, method, method);
			++total_unique_methods;
		}

		g_backtrace_frame_debug_lines[i] = debug_loc != NULL
			? debug_loc->row
			: 0;

		mono_debug_free_source_location(debug_loc);
	}

	return frame_count;
}
static void write_backtrace(
	FILE* file,
	const Accountant* acct,
	size_t frame_count,
	uint32_t& total_unique_backtraces)
{
	auto timestamp = get_ms_since_epoch();

	write_byte(file, cTagContext);
	write_pointer(file, acct->backtrace); // heapbuddy used to just write out 'acct' here...going with bt instead
	write_pointer(file, acct->klass);
	write_time(file, timestamp);

	assert(frame_count <= INT16_MAX);
	write_int16(file, frame_count);

	MonoDomain* domain = mono_domain_get();
	for (size_t i = 0; acct->backtrace[i] != NULL; ++i)
	{
		auto* frame = acct->backtrace[i];

		write_pointer(file, frame->method);
		//write_uint32(file, frame->il_offset);
		write_vuint(file, g_backtrace_frame_debug_lines[i]);
	}
	memset(g_backtrace_frame_debug_lines, 0, frame_count * sizeof(g_backtrace_frame_debug_lines[0]));

	++total_unique_backtraces;
}
void outfile_writer_add_accountant(
	OutfileWriter* ofw,
	Accountant* acct)
{
	// First, add this type if we haven't seen it before.
	ofw->write_class_if_not_already_seen(acct->klass);

	// Next, walk across the backtrace and add any previously-unseen methods.
	size_t frame_count = write_backtrace_methods(ofw->out, ofw->seen_items, acct->backtrace, ofw->total.method_count);

	// Now we can spew out the accountant's context
	write_backtrace(ofw->out, acct, frame_count, ofw->total.backtrace_count);

	ofw->try_flush();
}

// total_live_bytes is the total size of all of the live objects before the GC
void outfile_writer_gc_begin(
	OutfileWriter* ofw,
	gboolean       is_final,
	guint64        total_live_bytes,
	guint32        total_live_objects,
	guint32        n_accountants)
{
	auto timestamp = get_ms_since_epoch();

	write_byte(ofw->out, cTagGarbageCollect);
	write_int32(ofw->out, is_final ? -1 : ofw->total.gc_count);
	write_time(ofw->out, timestamp);
	write_uint64(ofw->out, total_live_bytes);
	write_uint32(ofw->out, total_live_objects);
	write_uint32(ofw->out, n_accountants);

	++ofw->total.gc_count;
}

void outfile_writer_gc_log_stats(
	OutfileWriter* ofw,
	Accountant* acct)
{
	write_pointer(ofw->out, acct->backtrace); // heapbuddy used to just write out 'acct' here...going with bt instead
	write_uint32(ofw->out, acct->n_allocated_objects);
	write_uint64(ofw->out, acct->n_allocated_bytes);
	write_uint32(ofw->out, acct->allocated_total_age);
	write_uint32(ofw->out, acct->allocated_total_weight);
	write_uint32(ofw->out, acct->n_live_objects);
	write_uint32(ofw->out, acct->n_live_bytes);
	write_uint32(ofw->out, acct->live_total_age);
	write_uint32(ofw->out, acct->live_total_weight);
}

// total_live_bytes is the total size of all live objects after the GC is finished
void outfile_writer_gc_end(
	OutfileWriter* ofw,
	guint64        total_allocated_bytes,
	guint32        total_allocated_objects,
	guint64        total_live_bytes,
	guint32        total_live_objects)
{
	write_uint64(ofw->out, total_live_bytes);
	write_uint32(ofw->out, total_live_objects);
	outfile_writer_update_totals(ofw, total_allocated_bytes, total_allocated_objects, true);

	ofw->try_flush();
}

void outfile_writer_resize(
	OutfileWriter* ofw,
	guint64        new_size,
	guint64        total_live_bytes,
	guint32        total_live_objects)
{
	auto timestamp = get_ms_since_epoch();

	write_byte(ofw->out, cTagResize);
	write_time(ofw->out, timestamp);
	write_uint64(ofw->out, new_size);
	write_uint64(ofw->out, total_live_bytes);
	write_uint32(ofw->out, total_live_objects);
	++ofw->total.resize_count;
	outfile_writer_update_totals(ofw, 0, 0, false);

	ofw->try_flush();
}

OutfileWriter::~OutfileWriter()
{
	g_hash_table_destroy(this->seen_items);
	this->seen_items = NULL;
}

uint64_t OutfileWriter::get_nanoseconds_offset() const
{
	return get_nanoseconds() - this->saved_outfile_nanos_start;
}

void OutfileWriter::try_flush()
{
	if (this->auto_flush)
		fflush(this->out);
}

void OutfileWriter::write_class_if_not_already_seen(MonoClass* klass)
{
	if (g_hash_table_lookup(this->seen_items, klass) == NULL)
	{
		write_mono_class(this->out, klass);

		g_hash_table_insert(this->seen_items, klass, klass);
		++this->total.type_count;
	}
}

void OutfileWriter::write_counts(guint64 total_allocated_bytes, guint32 total_allocated_objects, bool write_allocated_totals)
{
	write_uint32(this->out, this->total.gc_count);
	write_uint32(this->out, this->total.type_count);
	write_uint32(this->out, this->total.method_count);
	write_uint32(this->out, this->total.backtrace_count);
	write_uint32(this->out, this->total.resize_count);

	write_uint64(this->out, this->total.frames_count);
	write_uint64(this->out, this->total.object_news_count);
	write_uint64(this->out, this->total.object_resizes_count);
	write_uint64(this->out, this->total.object_gcs_count);
	write_uint64(this->out, this->total.boehm_news_count);
	write_uint64(this->out, this->total.boehm_frees_count);

	if (write_allocated_totals)
	{
		write_uint64(this->out, total_allocated_bytes);
		write_uint32(this->out, total_allocated_objects);
	}
}

void OutfileWriter::write_heap_stats(uint64_t heap_size, uint64_t heap_used_size)
{
	write_byte(this->out, cTagHeapSize);
	write_uint64(this->out, heap_size);
	write_uint64(this->out, heap_used_size);

	// intentionally not trying to flush here
}

void OutfileWriter::write_ios_memory_stats(ios_app_memory_stats* stats)
{
#if FALSE // iOS
	write_byte(this->out, cTagAppMemoryStats);
	write_uint64(this->out, stats->virtual_size);
	write_uint64(this->out, stats->resident_size);
	write_uint64(this->out, stats->resident_size_max);
#endif
}

void OutfileWriter::write_object_new(const MonoClass* klass, const MonoObject* obj, uint32_t size, gpointer backtrace)
{
#if HEAP_BOSS_TRACK_INDIVIDUAL_OBJECTS

	auto timestamp = get_ms_since_epoch();

	write_byte(this->out, cTagMonoObjectNew);
	write_time(this->out, timestamp);
	write_pointer(this->out, backtrace);
	//write_pointer(this->out, klass);
	write_pointer(this->out, obj);
	write_vuint(this->out, static_cast<uint32_t>(size));

	total.object_news_count++;

	// intentionally not trying to flush here
	// it would cause a flush on EVERY mono alloc
#endif
}
void OutfileWriter::write_object_resize(const MonoClass* klass, gpointer backtrace, const MonoObject* obj, uint32_t new_size)
{
#if HEAP_BOSS_TRACK_INDIVIDUAL_OBJECTS
	write_byte(this->out, cTagMonoObjectSizeChange);
	write_pointer(this->out, backtrace);
	//write_pointer(this->out, klass);
	write_pointer(this->out, obj);
	write_vuint(this->out, static_cast<uint32_t>(new_size));

	total.object_resizes_count++;

	// intentionally not trying to flush here
#endif
}
void OutfileWriter::write_object_gc(const MonoClass* klass, gpointer backtrace, const MonoObject* obj)
{
#if HEAP_BOSS_TRACK_INDIVIDUAL_OBJECTS
	write_byte(this->out, cTagMonoObjectGc);
	write_pointer(this->out, backtrace);
	//write_pointer(this->out, klass);
	write_pointer(this->out, obj);

	total.object_gcs_count++;

	// intentionally not trying to flush here
#endif
}

void OutfileWriter::write_custom_event(const char* event_string)
{
	auto timestamp = get_ms_since_epoch();

	write_byte(this->out, cTagCustomEvent);
	write_time(this->out, timestamp);
	write_string(this->out, event_string);

	this->try_flush();
}

void OutfileWriter::write_app_resign_active()
{
	auto timestamp = get_ms_since_epoch();

	write_byte(this->out, cTagAppResignActive);
	write_time(this->out, timestamp);

	this->try_flush();
}

void OutfileWriter::write_app_become_active()
{
	auto timestamp = get_ms_since_epoch();

	write_byte(this->out, cTagAppBecomeActive);
	write_time(this->out, timestamp);

	this->try_flush();
}

void OutfileWriter::write_new_frame()
{
	auto timestamp = get_ms_since_epoch();

	write_byte(this->out, cTagNewFrame);
	write_time(this->out, timestamp);

	total.frames_count++;
	this->try_flush();
}

void OutfileWriter::write_heap()
{
	heap_sections_written_count =
		heap_memory_total_heap_bytes = 
		heap_memory_total_bytes_written = 
		heap_memory_total_roots = 
		heap_memory_total_threads =
		0;

	write_byte(this->out, cTagHeapMemoryStart);

	fgetpos(this->out, &heap_memory_header_offset);
	write_uint32(this->out, heap_sections_written_count);
	write_uint32(this->out, heap_memory_total_heap_bytes);
	write_uint32(this->out, heap_memory_total_bytes_written);
	write_uint32(this->out, heap_memory_total_roots);
	write_uint32(this->out, heap_memory_total_threads);
}
void OutfileWriter::write_heap_end()
{
	write_byte(this->out, cTagHeapMemoryEnd);

	fpos_t old_fpos;
	fgetpos(this->out, &old_fpos);

	fsetpos(this->out, &heap_memory_header_offset);
	write_uint32(this->out, heap_sections_written_count);
	write_uint32(this->out, heap_memory_total_heap_bytes);
	write_uint32(this->out, heap_memory_total_bytes_written);
	write_uint32(this->out, heap_memory_total_roots);
	write_uint32(this->out, heap_memory_total_threads);

	fsetpos(this->out, &old_fpos);

	this->try_flush();
}

void OutfileWriter::write_heap_section(const void* start, const void* end)
{
	heap_section_blocks_written_count = 0;

	write_byte(this->out, cTagHeapMemorySection);
	write_pointer(this->out, start);
	write_pointer(this->out, end);

	fgetpos(this->out, &heap_section_header_offset);
	write_uint32(this->out, heap_section_blocks_written_count);
}
void OutfileWriter::write_heap_section_end()
{
	fpos_t old_fpos;
	fgetpos(this->out, &old_fpos);

	fsetpos(this->out, &heap_section_header_offset);
	write_uint32(this->out, heap_section_blocks_written_count);

	fsetpos(this->out, &old_fpos);
}

void OutfileWriter::write_heap_section_block(const void* start, size_t block_size, size_t obj_size, uint8_t block_kind, bool is_free)
{
	auto size = static_cast<uint32_t>(block_size);

	write_byte(this->out, cTagHeapMemorySectionBlock);
	write_pointer(this->out, start);
	write_uint32(this->out, size);
	write_uint32(this->out, static_cast<uint32_t>(obj_size));
	write_byte(this->out, block_kind);
	write_byte(this->out, is_free);

	if (!is_free)
	{
		fwrite(start, sizeof(char), size, this->out);
		heap_memory_total_bytes_written += size;
	}
	heap_memory_total_heap_bytes += size;

	heap_section_blocks_written_count++;
}

void OutfileWriter::write_heap_root_sets_start()
{
	write_byte(this->out, cTagHeapMemoryRoots);
}
void OutfileWriter::write_heap_root_set(const void* start, const void* end)
{
	// HACK: iOS port hack!
	if (heap_memory_total_roots == 0)
		write_heap_root_sets_start();

	write_pointer(this->out, start);
	write_pointer(this->out, end);

	size_t size = static_cast<size_t>((char*)end - (char*)start);
	if (size > 0)
	{
		if (size & 1) // fucking libgc requires +1 in the register static roots...
			size -= 1;

		fwrite(start, sizeof(char), size, this->out);
	}

	heap_memory_total_roots++;
}
void OutfileWriter::write_thread_stack(int32_t thread_id, const void* stack, size_t stack_size, const void* registers, size_t registers_size)
{
	if (heap_memory_total_threads == 0)
		write_byte(this->out, cTagHeapMemoryThreads);

	write_int32(this->out, thread_id);
	write_pointer(this->out, stack);
	write_vuint(this->out, static_cast<uint32_t>(stack_size));
	write_vuint(this->out, static_cast<uint32_t>(registers_size));

	if (stack_size > 0)
	{
		fwrite(stack, sizeof(char), stack_size, this->out);
	}

	if (registers_size > 0)
	{
		fwrite(registers, sizeof(char), registers_size, this->out);
	}

	heap_memory_total_threads++;
}

void OutfileWriter::write_boehm_allocation(gpointer address, size_t size)
{
#if HEAP_BOSS_TRACK_INDIVIDUAL_OBJECTS
	auto timestamp = get_ms_since_epoch();

	write_byte(this->out, cTagBoehmAlloc);
	write_time(this->out, timestamp);
	write_pointer(this->out, address);
	write_vuint(this->out, static_cast<uint32_t>(size));

	total.boehm_news_count++;

	// intentionally not trying to flush here
#endif
}
void OutfileWriter::write_boehm_free(gpointer address, size_t size)
{
#if HEAP_BOSS_TRACK_INDIVIDUAL_OBJECTS
	auto timestamp = get_ms_since_epoch();

	write_byte(this->out, cTagBoehmFree);
	write_time(this->out, timestamp);
	write_pointer(this->out, address);
	write_vuint(this->out, static_cast<uint32_t>(size));

	total.boehm_frees_count++;

	// intentionally not trying to flush here
#endif
}

void OutfileWriter::write_class_vtable_created(MonoDomain* domain, MonoClass* klass, MonoVTable* vtable)
{
	write_class_if_not_already_seen(klass);

	write_byte(this->out, cTagMonoVTable);
	write_pointer(this->out, vtable);
	write_pointer(this->out, klass);

	// intentionally not trying to flush here
}
void OutfileWriter::write_class_statics_allocation(MonoDomain* domain, MonoClass* klass, gpointer data, size_t data_size)
{
	write_class_if_not_already_seen(klass);

	write_byte(this->out, cTagMonoClassStatics);
	write_pointer(this->out, klass);
	write_pointer(this->out, data);
	write_vuint(this->out, static_cast<uint32_t>(data_size));

	try_flush();
}

void OutfileWriter::write_thread_table_allocation(MonoThread** table, size_t table_count, size_t table_size)
{
	write_byte(this->out, cTagMonoThreadTableResize);
	write_pointer(this->out, table);
	write_vuint(this->out, static_cast<uint32_t>(table_count));
	write_vuint(this->out, static_cast<uint32_t>(table_size));

	try_flush();
}
void OutfileWriter::write_thread_statics_allocation(gpointer data, size_t data_size)
{
	write_byte(this->out, cTagMonoThreadStatics);
	write_pointer(this->out, data);
	write_vuint(this->out, static_cast<uint32_t>(data_size));

	try_flush();
}