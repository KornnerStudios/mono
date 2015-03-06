#pragma once

struct _MonoClass;
struct StackFrame;

#include <list>

struct OutfileWriter;

struct LiveObject
{
	MonoObject* obj;
	uint32_t size;
	uint32_t age;
	uint32_t largest_size;
};

struct Accountant
{
	_MonoClass* klass;
	StackFrame** backtrace;

	guint32   n_allocated_objects;
	guint32   n_allocated_bytes;
	guint32   allocated_total_age;
	guint32   allocated_total_weight;
	guint32   n_live_objects;
	guint32   n_live_bytes;
	guint32   live_total_age;
	guint32   live_total_weight;

	std::list<LiveObject> live_objects;

	gboolean dirty;

	~Accountant();

private:
	void update_for_resize(LiveObject& live, uint32_t new_size);
public:
	void register_object(MonoObject* obj, uint32_t obj_size);
	void post_gc_processing(OutfileWriter* ofw);
};

Accountant* accountant_new(_MonoClass* klass, StackFrame** backtrace);