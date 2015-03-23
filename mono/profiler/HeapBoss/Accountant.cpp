
#include <mono/profiler/HeapBoss/Accountant.hpp>

extern "C"
{
extern gboolean mono_object_is_alive(MonoObject* obj);

#include <glib.h>
#include <mono/metadata/class.h>
#include <mono/metadata/object.h>
#include <mono/metadata/mono-gc.h>
#include <mono/metadata/debug-helpers.h>
}

#include <mono/profiler/HeapBoss/BackTrace.hpp>
#include <mono/profiler/HeapBoss/OutFileWriter.hpp>

Accountant::Accountant()
	: klass(NULL)
	, backtrace(NULL)
	, dirty(FALSE)
	, n_allocated_objects(0)
	, n_allocated_bytes(0)
	, allocated_total_age(0)
	, allocated_total_weight(0)
	, n_live_objects(0)
	, n_live_bytes(0)
	, live_total_age(0)
	, live_total_weight(0)
	, live_objects()
{
}

Accountant::~Accountant()
{
}

Accountant* accountant_new(MonoClass* klass, BackTraceByClass* backtrace)
{
	Accountant* acct = new Accountant();
	acct->klass = klass;
	acct->backtrace = backtrace;

	return acct;
}

void Accountant::register_object(MonoObject *obj, uint32_t obj_size)
{
	LiveObject live;

	live.obj = obj;
	live.size = obj_size;
	live.age = 0;
	live.largest_size = obj_size;

	this->live_objects.push_front(live);

	this->n_allocated_objects++;
	this->n_allocated_bytes += live.size;

	this->n_live_objects++;
	this->n_live_bytes += live.size;

	this->dirty = TRUE;
}

void Accountant::update_for_resize(LiveObject& live, uint32_t new_size)
{
	// adjust the live bytes to account for the new size
	this->n_live_bytes -= live.size;
	this->n_live_bytes += new_size;

	// if the resize was an alloc increase, increment the total allocated bytes count by the count of additional bytes
	if (new_size > live.size && new_size > live.largest_size)
	{
		live.largest_size = new_size;
		this->n_allocated_bytes += (new_size - live.size);
	}

	live.size = new_size;
}

void Accountant::post_gc_processing(OutfileWriter* ofw)
{
	if (this->live_objects.size() > 0)
		this->dirty = TRUE;

	for (auto iter = this->live_objects.begin(); iter != this->live_objects.end();)
	{
		auto& live = *iter;

		if (mono_object_is_alive(live.obj))
		{
			this->allocated_total_age++;
			live.age++;
			this->live_total_age++;

//#if HEAP_BOSS_TRACK_INDIVIDUAL_OBJECTS
			bool was_resized = false;
			auto current_size = mono_object_get_size(live.obj);
			if (current_size != live.size)
			{
				was_resized = true;
				this->update_for_resize(live, current_size);
			}
//#endif

			this->allocated_total_weight += live.size;
			this->live_total_weight += live.size;

			++iter;

#if HEAP_BOSS_TRACK_INDIVIDUAL_OBJECTS
			if (was_resized)
				ofw->write_object_resize(this->klass, this->backtrace, live.obj, current_size);
#endif

		}
		else
		{
			this->n_live_objects--;
			this->n_live_bytes -= live.size;
			this->live_total_age -= live.age;
			this->live_total_weight -= live.size * live.age;

#if HEAP_BOSS_TRACK_INDIVIDUAL_OBJECTS
			ofw->write_object_gc(this->klass, this->backtrace, live.obj);
#endif

			auto current = iter;
			++iter;
			this->live_objects.erase(current);
		}
	}
}