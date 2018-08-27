// SPDX-License-Identifier: GPL-2.0
#include "builtin.h"
#include "perf.h"

#include "util/util.h"
#include "util/evlist.h"
#include "util/cache.h"
#include "util/evsel.h"
#include "util/symbol.h"
#include "util/thread.h"
#include "util/header.h"
#include "util/session.h"
#include "util/tool.h"
#include "util/cloexec.h"
#include "util/thread_map.h"
#include "util/color.h"
#include "util/stat.h"
#include "util/callchain.h"
#include "util/time-utils.h"

#include <subcmd/parse-options.h>
#include "util/trace-event.h"

#include "util/debug.h"

#include <linux/kernel.h>
#include <linux/log2.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <inttypes.h>
#include <unistd.h>
#include <string.h>

#include <errno.h>
#include <semaphore.h>
#include <pthread.h>
#include <math.h>
#include <api/fs/fs.h>
#include <linux/time64.h>

#include "sane_ctype.h"

#define MAX_CPUS		4096
#define COMM_LEN		20
#define SYM_LEN			129
#define MAX_PID			1024000

struct func_atom {
	struct list_head	list;
	struct list_head	list_cpu;
	u64			entry_time;
	u64			exit_time;
	u64			runtime;
	bool		finish;
};

struct func_atoms {
	struct list_head	work_list;
	struct rb_node		node;
	int			key;
	u64			min_run;
	u64			min_run_at;
	u64			max_run;
	u64			max_run_at;
	u64			nb_atoms;  
	u64			total_runtime;

	unsigned long	 nr_lost_events;
};

struct func_class
{
	int 	 cpu_num;
	int		 max_cpu;  /*保存最大的cpu编号*/
	struct rb_root	 atom_root, sorted_atom_root, merged_atom_root;
	int 	 profile_cpu;
	int 	 profile_key;
	u64		 all_runtime;                 
	u64		 all_count;  
	struct func_class *func_cpu[MAX_CPUS];
};

struct thread_signal {
	u64 total_send;
	u64 total_recv;
	u64 total_error;

	u64 send_sig[64];
	u64 recv_sig[64];
	u64 error_sig[64];
};


struct perf_signal {
	struct perf_tool tool;
	const char	 *sort_order;
	const struct trace_sched_handler *tp_handler;  
	int		 profile_cpu;
	int 	 profile_irq;
	int 	 profile_softirq;
	struct	func_class irq_soc;
	struct	func_class softirq_soc;
	bool 	force;

	unsigned long	 nr_timestamps;
	unsigned long	 nr_unordered_timestamps;
	unsigned long	 nr_events;
	unsigned long	 nr_lost_chunks;
	unsigned long	 nr_lost_events;
	u64		 run_measurement_overhead;
	u64		 run_avg;
	struct list_head sort_list, cmp_pid;   /*sort_list是一个排序优先级链表，cmp_list是一个比较函数链表*/

};

struct perf_signal;
int fd_gv=-1;
	
static struct thread_signal *thread__init_signal(struct thread *thread)
{
	struct thread_signal *r;

	r = zalloc(sizeof(struct thread_signal));
	if (!r)
		return NULL;

	thread__set_priv(thread, r);

	return r;
}

static struct thread_signal *thread__get_signal(struct thread *thread)
{
	struct thread_signal *tr;

	tr = thread__priv(thread);
	if (tr == NULL) {
		tr = thread__init_signal(thread);
		if (tr == NULL)
			printf("Failed to malloc memory for runtime data.\n");
	}

	return tr;
}

static char *timehist_get_commstr(struct thread *thread)
{
	static char str[32];
	const char *comm = thread__comm_str(thread);
	pid_t tid = thread->tid;
	pid_t pid = thread->pid_;
	int n __maybe_unused;

	if (pid == 0)
		n = scnprintf(str, sizeof(str), "%s", comm);

	else if (tid != pid)
		n = scnprintf(str, sizeof(str), "%s[%d/%d]", comm, tid, pid);

	else
		n = scnprintf(str, sizeof(str), "%s[%d]", comm, tid);

	return str;
}

static int process_signal_generate_event(struct perf_tool *tool __maybe_unused,
					    struct perf_evsel *evsel,
					    struct perf_sample *sample,
					    struct machine *machine __maybe_unused)
{
//	struct perf_signal *signal = container_of(tool, struct perf_signal, tool);
	struct thread *thread,*thread_recv;
	struct thread_signal *tg = NULL;
	int recv_pid = perf_evsel__intval(evsel, sample, "pid");
	int sig = perf_evsel__intval(evsel, sample, "sig");
	int result = perf_evsel__intval(evsel, sample, "result");
	char const *head_gv = "digraph ptree {\nnode [ style = filled ];\n";
	char buf[256];

	thread = machine__findnew_thread(machine, sample->pid, sample->tid);
	if (thread == NULL) {
		printf("problem processing %d, skipping it.\n",sample->pid);
		return -1;
	}

	thread_recv = machine__findnew_thread(machine, recv_pid, recv_pid);
	if (thread_recv == NULL) {
		printf("problem processing %d, skipping it.\n",recv_pid);
		return -1;
	}

	tg = thread__get_signal(thread);
	if (tg == NULL)
		return -1;

	tg->send_sig[sig]++;
	tg->total_send++;
	if(result != 0)
		tg->error_sig[sig]++;

	if(fd_gv < 0)
	{
		fd_gv = open("signal.gv",O_CREAT|O_RDWR|O_TRUNC|O_CLOEXEC, S_IRUSR|S_IWUSR);
		if(fd_gv < 0)
		{
			printf("Can not open heat.data %d\n",fd_gv);
			return -1;
		}
		result = write(fd_gv,head_gv,strlen(head_gv));
	}
	result = sprintf(buf,"\"%d\" -> \"%d\" [ label = \"%d\" ];\n"
					   	 "\"%d\" [ label = \"%s\" ];\n",
						sample->pid,recv_pid,sig,
						sample->pid,timehist_get_commstr(thread));
	result = write(fd_gv,buf,result);

	result = sprintf(buf,"\"%d\" [ label = \"%s\" ];\n",
						recv_pid,timehist_get_commstr(thread_recv));
	result = write(fd_gv,buf,result);
	
	return 0;
}

static int process_signal_deliver_entry_event(struct perf_tool *tool __maybe_unused,
					  struct perf_evsel *evsel,
					  struct perf_sample *sample,
					  struct machine *machine __maybe_unused)
{
//	struct perf_signal *signal = container_of(tool, struct perf_signal, tool);
	struct thread *thread;
	struct thread_signal *tg = NULL;
	int sig = perf_evsel__intval(evsel, sample, "sig");

	thread = machine__findnew_thread(machine, sample->pid, sample->tid);
	if (thread == NULL) {
		printf("problem processing %d, skipping it.\n",sample->pid);
		return -1;
	}

	tg = thread__get_signal(thread);
	if (tg == NULL)
		return -1;

	tg->recv_sig[sig]++;
	tg->total_recv++;

	return 0;
}						

typedef int (*tracepoint_handler)(struct perf_tool *tool,
				  struct perf_evsel *evsel,
				  struct perf_sample *sample,
				  struct machine *machine);

static int perf_signal__process_tracepoint_sample(struct perf_tool *tool __maybe_unused,
						 union perf_event *event __maybe_unused,
						 struct perf_sample *sample,
						 struct perf_evsel *evsel,
						 struct machine *machine)
{
	int err = 0;

	if (evsel->handler != NULL) {
		tracepoint_handler f = evsel->handler;
		err = f(tool, evsel, sample, machine);
	}

	return err;
}

static int show_thread_signal(struct thread *t, void *priv __maybe_unused)
{
	struct thread_signal *r;
	int i;
	
	if (thread__is_filtered(t))
		return 0;

	r = thread__priv(t);
	if(r != NULL)
	{
		printf("%30s :\t%8lld \t%8lld \t%8lld\n",timehist_get_commstr(t),r->total_send,r->total_recv,r->total_error);
		for(i=0; i<64; i++)
		{
			if(r->send_sig[i] != 0 || r->recv_sig[i] != 0 || r->error_sig[i] != 0)
				printf("                               |->[%02d] \t   %8lld \t   %8lld \t   %8lld\n",i,r->send_sig[i],r->recv_sig[i],r->error_sig[i]);
		}
	}
	return 0;
}


static void signal_report_print(struct perf_signal *signal  __maybe_unused, struct perf_session *session)
{
	struct machine *m = &session->machines.host;

	printf("\n ------------------------------------------------------------------------------------\n");
	printf("           comm                |  SIG  |      Send     |      Recv     |      Error   \n");
	printf(" -------------------------------------------------------------------------------------\n");

	machine__for_each_thread(m, show_thread_signal, NULL);

	printf(" -------------------------------------------------------------------------------------\n");

	printf("\n");

}

static int perf_signal__report(struct perf_signal *signal) 
{
	const struct perf_evsel_str_handler handlers[] = {
		{ "signal:signal_deliver",		  process_signal_deliver_entry_event, },
		{ "signal:signal_generate", 	  process_signal_generate_event, },
	};
	struct perf_session *session;
	struct perf_data_file file = {
		.path = input_name,
		.mode = PERF_DATA_MODE_READ,
		.force = signal->force,
	};
	int rc = -1;

	setup_pager();

	session = perf_session__new(&file, false, &signal->tool);
	if (session == NULL) {
		pr_debug("No Memory for session\n");
		return -1;
	}

	symbol__init(&session->header.env);

	if (perf_session__set_tracepoints_handlers(session, handlers)) 
		goto out_delete;

	if (perf_session__has_traces(session, "record -R")) {
		int err = perf_session__process_events(session);  
		if (err) {
			pr_err("Failed to process events, error %d", err);
			goto out_delete;
		}

		signal->nr_events	   = session->evlist->stats.nr_events[0];
		signal->nr_lost_events = session->evlist->stats.total_lost;
		signal->nr_lost_chunks = session->evlist->stats.nr_events[PERF_RECORD_LOST];
	}

	signal_report_print(signal,session);

	rc = write(fd_gv,"}\n",2);
	rc= system("neato -Tpng -Nfontsize=12 -Elen=1.9 signal.gv -o signal.png");
	printf("generate signal.png\n");
	
	return 0;
out_delete:
	perf_session__delete(session);
	return rc;

}

static int __cmd_record(int argc, const char **argv)
{
	unsigned int rec_argc, i, j;
	const char **rec_argv;
	const char * const record_args[] = {
		"record",
		"-a",
		"-R",
		"-m", "1024",
		"-c", "1",
		"-e", "signal:signal_deliver",
		"-e", "signal:signal_generate ",
	};

	rec_argc = ARRAY_SIZE(record_args) + argc - 1;
	rec_argv = calloc(rec_argc + 1, sizeof(char *));

	if (rec_argv == NULL)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(record_args); i++)
		rec_argv[i] = strdup(record_args[i]);

	for (j = 1; j < (unsigned int)argc; j++, i++)
		rec_argv[i] = argv[j];

	BUG_ON(i != rec_argc);

	return cmd_record(i, rec_argv);
}

int cmd_signal(int argc, const char **argv)
{
	struct perf_signal signal = {
		.tool = {
			.sample		 = perf_signal__process_tracepoint_sample,
			.comm		 = perf_event__process_comm,
			.exit	 	 = perf_event__process_exit,
			.fork	 	 = perf_event__process_fork,
			.namespaces	 = perf_event__process_namespaces,
			.lost		 = perf_event__process_lost,
			.attr    	 = perf_event__process_attr,
			.tracing_data = perf_event__process_tracing_data,
			.build_id	 = perf_event__process_build_id,			
			.ordered_events = true,
		},
		.cmp_pid	      = LIST_HEAD_INIT(signal.cmp_pid),
		.sort_list	      = LIST_HEAD_INIT(signal.sort_list),
//		.sort_order	      = default_sort_order,
		.profile_cpu	      = -1,
		.profile_irq	      = -1,
		.profile_softirq		  = -1,

	};
	const struct option signal_options[] = {
	OPT_STRING('i', "input", &input_name, "file",
		    "input file name"),
	OPT_INCR('v', "verbose", &verbose,
		    "be more verbose (show symbol address, etc)"),
	OPT_BOOLEAN('D', "dump-raw-trace", &dump_trace,
		    "dump raw trace in ASCII"),
	OPT_BOOLEAN('f', "force", &signal.force, "don't complain, do it"),
	OPT_END()
	};
	const struct option report_options[] = {
	OPT_STRING('s', "sort", &signal.sort_order, "key[,key2...]",
		   "sort by key(s): signal, avg, max, min, count, runtime"),
	OPT_INTEGER('C', "CPU", &signal.profile_cpu,
		    "CPU to profile on"),
	OPT_INTEGER('H', "signal", &signal.profile_irq,
		    "set signal to profile"),
	OPT_INTEGER('S', "softirq", &signal.profile_softirq,
		    "set softirq to profile"),
	OPT_PARENT(signal_options)
	};

	const char * const report_usage[] = {
		"perf signal report [<options>]",
		NULL
	};

	const char *const signal_subcommands[] = { "record", "report", NULL };
	const char *signal_usage[] = {
		NULL,
		NULL
	};

	argc = parse_options_subcommand(argc, argv, signal_options, signal_subcommands,
					signal_usage, PARSE_OPT_STOP_AT_NON_OPTION);
	if (!argc)
		usage_with_options(signal_usage, signal_options);

	if (!strncmp(argv[0], "rec", 3)) {
		return __cmd_record(argc, argv);
	} else if (!strncmp(argv[0], "rep", 3)) {
		if (argc > 1) {
			argc = parse_options(argc, argv, report_options, report_usage, 0);
			if (argc)
				usage_with_options(report_usage, report_options);
		}
		return perf_signal__report(&signal);
	} else {
		usage_with_options(signal_usage, signal_options);
	}

	return 0;
}
