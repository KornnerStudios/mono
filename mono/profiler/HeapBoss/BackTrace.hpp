#pragma once

struct _MonoMethod;

struct StackFrame
{
	_MonoMethod *method;
	gint32 native_offset;
	//gint32 il_offset;
	//gboolean managed;
};

void backtrace_cache_initialize();
void backtrace_cache_dispose();

StackFrame** backtrace_get_current();
