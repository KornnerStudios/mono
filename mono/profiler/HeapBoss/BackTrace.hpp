#pragma once

#include <vector>
#include <unordered_map>

struct _MonoMethod;
struct _MonoClass;
struct _GHashTable;

struct StackFrame
{
	_MonoMethod *method;
	gint32 native_offset;
	//gint32 il_offset;
	//gboolean managed;
};

struct BackTrace;

struct BackTraceByClass
{
	_MonoClass* klass;
	const BackTrace* owner;

	BackTraceByClass(_MonoClass* klass, const BackTrace* backtrace);
};

struct BackTrace
{
	std::vector<StackFrame> frames;
	std::unordered_map<_MonoClass*, BackTraceByClass*> klass_backtraces;

	BackTrace(size_t frameCount);
	~BackTrace();

	BackTraceByClass* get_or_add_class(_MonoClass* klass);
};

void backtrace_cache_initialize();
void backtrace_cache_dispose();

BackTraceByClass*
backtrace_get_current(MonoObject* obj, MonoClass* klass);
