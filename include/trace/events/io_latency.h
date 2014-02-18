#undef TRACE_SYSTEM
#define TRACE_SYSTEM io_latency

#if !defined(_TRACE_IO_LATENCY_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_IO_LATENCY_H

#include <linux/tracepoint.h>

TRACE_EVENT(io_latency_entry,

	TP_PROTO(u64 latency, u64 avg_latency),

	TP_ARGS(latency, avg_latency),

	TP_STRUCT__entry(
		__field(	u64,		latency		)
		__field(	u64,		avg_latency	)
	),

	TP_fast_assign(
		__entry->latency = latency;
		__entry->avg_latency = avg_latency;
	),

       TP_printk("latency=%llu, avg latency=%llu",
		 __entry->latency, __entry->avg_latency)
);

#endif /* _TRACE_IO_LATENCY_H */

/* This part must be outside protection */
#include <trace/define_trace.h>
