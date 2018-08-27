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

struct perf_irq {
	struct perf_tool tool;
	const char	 *sort_order;
	const struct trace_sched_handler *tp_handler;  /*latancy，timehist等都是不一样的*/
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

struct perf_irq;

typedef int (*sort_fn_t)(struct func_atoms *, struct func_atoms *);

struct sort_dimension {
	const char		*name;
	sort_fn_t		cmp;
	struct list_head	list;
};

static int
thread_irq_cmp(struct list_head *list, struct func_atoms *l, struct func_atoms *r)
{
	struct sort_dimension *sort;
	int ret = 0;

	BUG_ON(list_empty(list));

	list_for_each_entry(sort, list, list) {
		ret = sort->cmp(l, r);
		if (ret)
			return ret;
	}

	return ret;
}

static struct func_atoms *
thread_atoms_search(struct rb_root *root, int key,
			 struct list_head *sort_list)
{
	struct rb_node *node = root->rb_node;
	struct func_atoms key_atoms = { .key = key };

	while (node) {
		struct func_atoms *atoms;
		int cmp;

		atoms = container_of(node, struct func_atoms, node);

		cmp = thread_irq_cmp(sort_list, &key_atoms, atoms);
		if (cmp > 0)
			node = node->rb_left;
		else if (cmp < 0)
			node = node->rb_right;
		else {
			BUG_ON(key != atoms->key);
			return atoms;
		}
	}
	return NULL;
}

static void
__thread_atoms_insert(struct rb_root *root, struct func_atoms *data,
			 struct list_head *sort_list)
{
	struct rb_node **new = &(root->rb_node), *parent = NULL;

	while (*new) {
		struct func_atoms *this;
		int cmp;

		this = container_of(*new, struct func_atoms, node);
		parent = *new;

		cmp = thread_irq_cmp(sort_list, data, this);

		if (cmp > 0)
			new = &((*new)->rb_left);
		else
			new = &((*new)->rb_right);
	}

	rb_link_node(&data->node, parent, new);
	rb_insert_color(&data->node, root);
}

static int thread_atoms_insert(struct rb_root *root, int key, struct list_head *sort_list)
{
	struct func_atoms *atoms = zalloc(sizeof(*atoms));
	if (!atoms) {
		pr_err("No memory at %s\n", __func__);
		return -1;
	}

	atoms->key = key;
	INIT_LIST_HEAD(&atoms->work_list);
	__thread_atoms_insert(root, atoms, sort_list);
	return 0;
}

static int
add_func_enter_event(struct func_atoms *atoms, struct func_atoms *atoms_cpu, u64 timestamp)
{
	struct func_atom *atom,*atom_new;
	char buf[32];

	if(!list_empty(&atoms_cpu->work_list))
	{
		atom = list_last_entry(&atoms_cpu->work_list, struct func_atom, list_cpu);
		if(atom->finish != 1)
		{
			list_del(&atom->list_cpu);
			atoms_cpu->nb_atoms--;
			atoms_cpu->nr_lost_events++;

			list_del(&atom->list);
			atoms->nb_atoms--;
			atoms->nr_lost_events++;

			timestamp__scnprintf_usec(timestamp, buf, sizeof(buf));
			pr_err("func with out exit %s at %s\n", __func__, buf);
		}	
	}
	
	atom_new = zalloc(sizeof(*atom_new));
	if (!atom_new) {
		pr_err("Non memory at %s", __func__);
		return -1;
	}
	
	atom_new->entry_time = timestamp;
	atom_new->finish = 0;

	atoms->nb_atoms++;
	list_add_tail(&atom_new->list, &atoms->work_list);

	atoms_cpu->nb_atoms++;
	list_add_tail(&atom_new->list_cpu, &atoms_cpu->work_list);

	return 0;
}

static int
add_func_exit_event(struct func_atoms *atoms, struct func_atoms *atoms_cpu, u64 timestamp)
{
	s64 delta=0;
	char buf[32];
	struct func_atom *atom;

	if(list_empty(&atoms_cpu->work_list))
		return 0;
		
	atom = list_last_entry(&atoms_cpu->work_list, struct func_atom, list_cpu);

	if(atom->finish == 1)
	{
		timestamp__scnprintf_usec(timestamp, buf, sizeof(buf));
		pr_err("func with out enter %s at %s, drop this one\n", __func__,buf );
		atoms->nr_lost_events++;
		atoms_cpu->nr_lost_events++;
		return 0;
	}

	atom->exit_time = timestamp;

	delta = timestamp - atom->entry_time;
	if (delta < 0) {
		pr_err("runtime, delta: %" PRIu64 " < 0 ?\n", delta);
		list_del(&atom->list);
		atoms->nb_atoms--;
		atoms->nr_lost_events++;

		list_del(&atom->list_cpu);
		atoms_cpu->nb_atoms--;
		atoms_cpu->nr_lost_events++;
		return 0;
	}

	atom->runtime = delta;
	atom->finish = 1;
	if(atom->runtime > atoms->max_run)
	{
		atoms->max_run = atom->runtime;
		atoms->max_run_at = atom->entry_time;
	}
	if((atom->runtime < atoms->min_run) || atoms->min_run == 0)
	{
		atoms->min_run = atom->runtime;
		atoms->min_run_at = atom->entry_time;
	}
	atoms->total_runtime += atom->runtime;


	if(atom->runtime > atoms_cpu->max_run)
	{
		atoms_cpu->max_run = atom->runtime;
		atoms_cpu->max_run_at = atom->entry_time;
	}
	if((atom->runtime < atoms_cpu->min_run) || atoms_cpu->min_run == 0)
	{
		atoms_cpu->min_run = atom->runtime;
		atoms_cpu->min_run_at = atom->entry_time;
	}
	atoms_cpu->total_runtime += atom->runtime;

	return 0;
}


static int function_enter_event(struct func_class *funclass,
				struct perf_evsel *evsel __maybe_unused,
				struct perf_sample *sample,
				struct machine *machine __maybe_unused,
				const char *key_name __maybe_unused,
				struct list_head *sort_list)
{
	const u32 key = perf_evsel__intval(evsel, sample, key_name);

	struct func_atoms *func_events,*func_events_cpu=NULL;
	u64 timestamp = sample->time;
	int cpu = sample->cpu, err = -1;

	BUG_ON(cpu >= MAX_CPUS || cpu < 0);
	if(cpu > funclass->max_cpu)
		funclass->max_cpu = cpu;

	if(funclass->func_cpu[cpu] == NULL)
	{
		funclass->func_cpu[cpu] = zalloc(sizeof(struct func_class));
		if (funclass->func_cpu[cpu] == NULL) {
			pr_err("No memory at %s\n", __func__);
			return -1;
		}
	}
	funclass->func_cpu[cpu]->cpu_num = cpu;

	func_events = thread_atoms_search(&funclass->atom_root, key, sort_list);
	if (!func_events) {
		if (thread_atoms_insert(&funclass->atom_root, key, sort_list))
			goto out_put;
		func_events = thread_atoms_search(&funclass->atom_root, key, sort_list);
		if (!func_events) {
			pr_err("func-event: Internal tree error");
			goto out_put;
		}
	}

	func_events_cpu = thread_atoms_search(&funclass->func_cpu[cpu]->atom_root, key, sort_list);
	if (!func_events_cpu) {
		if (thread_atoms_insert(&funclass->func_cpu[cpu]->atom_root, key, sort_list))
			goto out_put;
		func_events_cpu = thread_atoms_search(&funclass->func_cpu[cpu]->atom_root, key, sort_list);
		if (!func_events_cpu) {
			pr_err("func-event-cpu: Internal tree error");
			goto out_put;
		}
	}

	if (add_func_enter_event(func_events, func_events_cpu, timestamp)) 
		return -1;

	err = 0;
out_put:

	return err;
}

static int function_exit_event(struct func_class *funclass,
				struct perf_evsel *evsel __maybe_unused,
				struct perf_sample *sample,
				struct machine *machine __maybe_unused,
				const char *key_name __maybe_unused,
				struct list_head *sort_list)

{
	const u32 key = perf_evsel__intval(evsel, sample, key_name);
	struct func_atoms *func_events,*func_events_cpu=NULL;
	u64 timestamp = sample->time;
	int cpu = sample->cpu, err = -1;

	BUG_ON(cpu >= MAX_CPUS || cpu < 0);
	if(cpu > funclass->max_cpu)
		funclass->max_cpu = cpu;

	if(funclass->func_cpu[cpu] == NULL)
	{
		funclass->func_cpu[cpu] = zalloc(sizeof(struct func_class));
		if (funclass->func_cpu[cpu] == NULL) {
			pr_err("No memory at %s\n", __func__);
			return -1;
		}
	}
	funclass->func_cpu[cpu]->cpu_num = cpu;

	func_events = thread_atoms_search(&funclass->atom_root, key, sort_list);
	if (!func_events) {
		if (thread_atoms_insert(&funclass->atom_root, key, sort_list))
			goto out_put;
		func_events = thread_atoms_search(&funclass->atom_root, key, sort_list);
		if (!func_events) {
			pr_err("func-event: Internal tree error");
			goto out_put;
		}
	}

	func_events_cpu = thread_atoms_search(&funclass->func_cpu[cpu]->atom_root, key, sort_list);
	if (!func_events_cpu) {
		if (thread_atoms_insert(&funclass->func_cpu[cpu]->atom_root, key, sort_list))
			goto out_put;
		func_events_cpu = thread_atoms_search(&funclass->func_cpu[cpu]->atom_root, key, sort_list);
		if (!func_events_cpu) {
			pr_err("func-event-cpu: Internal tree error");
			goto out_put;
		}
	}

	if (add_func_exit_event(func_events, func_events_cpu,timestamp)) 
		return -1;

	err = 0;
out_put:

	return err;
}

static void output_irq_thread(struct func_class *func_class, struct func_atoms *irq_list)
{
	u64 avg;
	char max_lat_at[32];
	char min_lat_at[32];

	if (!irq_list->nb_atoms)
		return;

	func_class->all_runtime += irq_list->total_runtime;
	func_class->all_count   += irq_list->nb_atoms;

	printf("%4d    ", irq_list->key);

	avg = irq_list->total_runtime / irq_list->nb_atoms;
	timestamp__scnprintf_usec(irq_list->max_run_at, max_lat_at, sizeof(max_lat_at));
	timestamp__scnprintf_usec(irq_list->min_run_at, min_lat_at, sizeof(min_lat_at));

	printf("|%9" PRIu64 " |%11.3f ms | %9.3f ms |min %9.3f ms |%13ss|max %9.3f ms |%13ss\n",
		 irq_list->nb_atoms,
		 (double)irq_list->total_runtime / NSEC_PER_MSEC,
		 (double)avg / NSEC_PER_MSEC,
		 (double)irq_list->min_run / NSEC_PER_MSEC,
		 min_lat_at,
		 (double)irq_list->max_run / NSEC_PER_MSEC,
		 max_lat_at);
}

static int key_cmp(struct func_atoms *l, struct func_atoms *r)
{
	if (l->key == r->key)
		return 0;
	else if (l->key > r->key)
		return -1;
	else 
		return 1;
}

static int avg_cmp(struct func_atoms *l, struct func_atoms *r)
{
	u64 avgl, avgr;

	if (!l->nb_atoms)
		return -1;

	if (!r->nb_atoms)
		return 1;

	avgl = l->total_runtime / l->nb_atoms;
	avgr = r->total_runtime / r->nb_atoms;

	if (avgl < avgr)
		return -1;
	if (avgl > avgr)
		return 1;

	return 0;
}

static int max_cmp(struct func_atoms *l, struct func_atoms *r)
{
	if (l->max_run < r->max_run)
		return -1;
	if (l->max_run > r->max_run)
		return 1;

	return 0;
}

static int min_cmp(struct func_atoms *l, struct func_atoms *r)
{
	if (l->max_run < r->max_run)
		return 1;
	if (l->max_run > r->max_run)
		return -1;

	return 0;
}

static int count_cmp(struct func_atoms *l, struct func_atoms *r)
{
	if (l->nb_atoms < r->nb_atoms)
		return -1;
	if (l->nb_atoms > r->nb_atoms)
		return 1;

	return 0;
}

static int runtime_cmp(struct func_atoms *l, struct func_atoms *r)
{
	if (l->total_runtime < r->total_runtime)
		return -1;
	if (l->total_runtime > r->total_runtime)
		return 1;

	return 0;
}

static int sort_dimension__add(const char *tok, struct list_head *list)
{
	size_t i;
	static struct sort_dimension avg_sort_dimension = {
		.name = "avg",
		.cmp  = avg_cmp,
	};
	static struct sort_dimension max_sort_dimension = {
		.name = "max",
		.cmp  = max_cmp,
	};
	static struct sort_dimension min_sort_dimension = {
		.name = "min",
		.cmp  = min_cmp,
	};
	static struct sort_dimension irq_sort_dimension = {
		.name = "irq",
		.cmp  = key_cmp,
	};
	static struct sort_dimension runtime_sort_dimension = {
		.name = "runtime",
		.cmp  = runtime_cmp,
	};
	static struct sort_dimension count_sort_dimension = {
		.name = "count",
		.cmp  = count_cmp,
	};
		
	struct sort_dimension *available_sorts[] = {
		&irq_sort_dimension,
		&avg_sort_dimension,
		&max_sort_dimension,
		&min_sort_dimension,
		&runtime_sort_dimension,
		&count_sort_dimension,
	};

	for (i = 0; i < ARRAY_SIZE(available_sorts); i++) {
		if (!strcmp(available_sorts[i]->name, tok)) {
			list_add_tail(&available_sorts[i]->list, list);

			return 0;
		}
	}

	return -1;
}

static void function_sort_report(struct list_head *sort_list, struct func_class *func_class)
{
	struct rb_node *node;
	struct rb_root *root = &func_class->atom_root;

	for (;;) {
		struct func_atoms *data;
		node = rb_first(root);
		if (!node)
			break;

		rb_erase(node, root);
		data = rb_entry(node, struct func_atoms, node);
		__thread_atoms_insert(&func_class->sorted_atom_root, data, sort_list);
	}
}


static int process_irq_handler_entry_event(struct perf_tool *tool,
					    struct perf_evsel *evsel,
					    struct perf_sample *sample,
					    struct machine *machine)
{
	struct perf_irq *irq = container_of(tool, struct perf_irq, tool);

	return function_enter_event(&irq->irq_soc, evsel, sample, machine, "irq", &irq->cmp_pid);
}

static int process_irq_handler_exit_event(struct perf_tool *tool,
					  struct perf_evsel *evsel,
					  struct perf_sample *sample,
					  struct machine *machine)
{
	struct perf_irq *irq = container_of(tool, struct perf_irq, tool);

	return function_exit_event(&irq->irq_soc, evsel, sample, machine, "irq", &irq->cmp_pid);
}						

static int process_softirq_entry_event(struct perf_tool *tool,
					    struct perf_evsel *evsel,
					    struct perf_sample *sample,
					    struct machine *machine)
{
	struct perf_irq *irq = container_of(tool, struct perf_irq, tool);

	return function_enter_event(&irq->softirq_soc, evsel, sample, machine, "vec", &irq->cmp_pid);
}

static int process_softirq_exit_event(struct perf_tool *tool,
					  struct perf_evsel *evsel,
					  struct perf_sample *sample,
					  struct machine *machine)
{
	struct perf_irq *irq = container_of(tool, struct perf_irq, tool);

	return function_exit_event(&irq->softirq_soc, evsel, sample, machine, "vec", &irq->cmp_pid);
}						

typedef int (*tracepoint_handler)(struct perf_tool *tool,
				  struct perf_evsel *evsel,
				  struct perf_sample *sample,
				  struct machine *machine);

static int perf_irq__process_tracepoint_sample(struct perf_tool *tool __maybe_unused,
						 union perf_event *event __maybe_unused,
						 struct perf_sample *sample,
						 struct perf_evsel *evsel,
						 struct machine *machine)
{
	int err = 0;

	if (evsel->handler != NULL) {
		tracepoint_handler f = evsel->handler;    /*不同的tracepoint的分析函数是不一样的*/
		err = f(tool, evsel, sample, machine);
	}

	return err;
}

static int perf_irq__read_events(struct perf_irq *irq)
{
	const struct perf_evsel_str_handler handlers[] = {
		{ "irq:irq_handler_entry",	      process_irq_handler_entry_event, },
		{ "irq:irq_handler_exit", 		  process_irq_handler_exit_event, },
		{ "irq:softirq_entry",		  	  process_softirq_entry_event, },
		{ "irq:softirq_exit",		  	  process_softirq_exit_event, },
	};
	struct perf_session *session;
	struct perf_data_file file = {
		.path = input_name,
		.mode = PERF_DATA_MODE_READ,
		.force = irq->force,
	};
	int rc = -1;

	session = perf_session__new(&file, false, &irq->tool);
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

		irq->nr_events      = session->evlist->stats.nr_events[0];
		irq->nr_lost_events = session->evlist->stats.total_lost;
		irq->nr_lost_chunks = session->evlist->stats.nr_events[PERF_RECORD_LOST];
	}

	rc = 0;
out_delete:
	perf_session__delete(session);
	return rc;
}

#if 0
static void print_bad_events(struct perf_irq *irq)
{
	if (irq->nr_unordered_timestamps && irq->nr_timestamps) {
		printf("  INFO: %.3f%% unordered timestamps (%ld out of %ld)\n",
			(double)irq->nr_unordered_timestamps/(double)irq->nr_timestamps*100.0,
			irq->nr_unordered_timestamps, irq->nr_timestamps);
	}
	if (irq->nr_lost_events && irq->nr_events) {
		printf("  INFO: %.3f%% lost events (%ld out of %ld, in %ld chunks)\n",
			(double)irq->nr_lost_events/(double)irq->nr_events * 100.0,
			irq->nr_lost_events, irq->nr_events, irq->nr_lost_chunks);
	}
	if (irq->nr_context_switch_bugs && irq->nr_timestamps) {
		printf("  INFO: %.3f%% context switch bugs (%ld out of %ld)",
			(double)irq->nr_context_switch_bugs/(double)irq->nr_timestamps*100.0,
			irq->nr_context_switch_bugs, irq->nr_timestamps);
		if (irq->nr_lost_events)
			printf(" (due to lost events?)");
		printf("\n");
	}
}
#endif
static void print_hist(u64 *data,long len)
{
	u64 max=0,min=0,dela;
	u64 baseline[11];
	long count[10];
	long i,label,j;
	long line;
	int distribution;
	
	for(i=0; i<len; i++)
	{
		if(max < data[i])
			max = data[i];

		if(min ==0 || min > data[i])
			min = data[i];
	}

	dela = (max - min)/10;
	if(dela > NSEC_PER_MSEC)
	{
		line = NSEC_PER_MSEC;
		printf("       msecs           : count\t\t distribution\n");
	}
	else
	{
		line = NSEC_PER_USEC;
		printf("      usecs            : count\t\t distribution\n");
	}
	for(i=0;i<10;i++)
		baseline[i] = min + dela*i;
	baseline[10] = max;

	for(i=0;i<10;i++)
		count[i] = 0;

	for(i=0;i<len;i++)
	{
		label = 0;
		if(dela != 0)
			label = (data[i] - min)/dela;
		if(label >= 9)
			label = 9;
		count[label]++;
	}

	for(i=0;i<10;i++)
	{
		printf("%9.3f-%-9.3f    : %ld\t\t|",(double)baseline[i]/line, (double)baseline[i+1]/line,count[i]);
		distribution = (count[i] * 50 / len) + (count[i] ? 1 : 0); 
		for(j=0;j<50;j++)
		{
			if(j<distribution)
			printf("*");
			else
			printf(" ");
		}
		printf("|\n");
	}
	printf("analyes %ld [max:%lld  min:%lld]\n",len,max,min);
	
}

static void function_report_print_by_key(struct func_class *func_class_top, struct list_head *sort_list)
{
	struct func_atoms *func_events=NULL;
	struct func_class *func_class_want = NULL;
	struct func_atom *func_node;
	u64 *data_buf;
	long cnt=0;
	int fd,ret;
	char buf[128];
	u64 time_begin=0,time_end=0,run_max=0,run_min=0;

	if(func_class_top->profile_key < 0)
	{
		printf("Must define profile key \n");
		return;
	}

	if(func_class_top->profile_cpu >= 0 && func_class_top->profile_cpu <= func_class_top->max_cpu)
	{
		func_class_want = func_class_top->func_cpu[func_class_top->profile_cpu];
		if(func_class_want == NULL)
		{
			printf("Key %d on cpu %d don't exit\n", func_class_top->profile_key, func_class_top->profile_cpu);
			return;
		}
	}
	else
		func_class_want = func_class_top;

	func_events = thread_atoms_search(&func_class_want->atom_root, func_class_top->profile_key, sort_list);
	if(func_events == NULL)
	{
		printf("Could not find key %d\n",func_class_top->profile_key);
		return;
	}

	data_buf = malloc(sizeof(u64) * func_events->nb_atoms);
	if(data_buf == NULL)
	{
		printf("Alloc mem error\n");
		return ;
	}

	fd = open("heat.data",O_CREAT|O_RDWR|O_TRUNC|O_CLOEXEC, S_IRUSR|S_IWUSR);
	if(fd < 0)
	{
		printf("Can not open heat.data %d\n",fd);
		return;
	}
	
	if(func_class_top->profile_cpu >= 0 )
	{
		list_for_each_entry(func_node, &func_events->work_list, list_cpu) {
			if(time_begin == 0)
				time_begin = func_node->entry_time;
			time_end = func_node->entry_time;
			if(func_node->runtime > run_max)
				run_max = func_node->runtime;
			if(func_node->runtime < run_min || run_min == 0)
				run_min = func_node->runtime;
			ret = sprintf(buf,"%lld %lld\n",func_node->entry_time / NSEC_PER_MSEC,func_node->runtime);
			ret = write(fd,buf,ret);
//			printf("%9.3f ms\n",(double)func_node->runtime / NSEC_PER_MSEC);
			data_buf[cnt++] = func_node->runtime;
		}
	}
	else {
		list_for_each_entry(func_node, &func_events->work_list, list) {
			if(time_begin == 0)
				time_begin = func_node->entry_time;
			time_end = func_node->entry_time;
			if(func_node->runtime > run_max)
				run_max = func_node->runtime;
			if(func_node->runtime < run_min || run_min == 0)
				run_min = func_node->runtime;
			ret = sprintf(buf,"%lld %lld\n",func_node->entry_time / NSEC_PER_MSEC,func_node->runtime);
			ret = write(fd,buf,ret);
//			printf("%9.3f ms\n",(double)func_node->runtime / NSEC_PER_MSEC);
			data_buf[cnt++] = func_node->runtime;
		}
	}
	close(fd);
	print_hist(data_buf,cnt);

	ret = sprintf(buf,"tools/trace2heatmap.pl --title \"irq %d\" --unitstime=ms --unitslabel=ns --maxlat=%lld --minlat=%lld --grid heat.data > heat.svg ",
		func_class_top->profile_key, run_max, run_min);
	printf("%lld-%lld:%s\n",time_begin,time_end,buf);
	ret = system(buf);
}

static void function_report_print(struct list_head *sort_list, struct func_class *func_class)
{
	struct rb_node *next;

	function_sort_report(sort_list,func_class);   

	printf("\n ---------------------------------------------------------------------------------------------------------------------\n");
	printf("  IRQ   |   count  | Total run ms  |  Avg run ms  | Minimun run ms  |  Mini run at  | Maximun run ms  |  Maxi run at  \n");
	printf(" ---------------------------------------------------------------------------------------------------------------------\n");

	next = rb_first(&func_class->sorted_atom_root);

	while (next) {
		struct func_atoms *work_list;

		work_list = rb_entry(next, struct func_atoms, node);
		output_irq_thread(func_class, work_list);
		next = rb_next(next);
	}

	printf(" ---------------------------------------------------------------------------------------------------------------------\n");
	printf(" TOTAL: |%9" PRIu64 " |%11.3f ms |\n",
		func_class->all_count,(double)func_class->all_runtime / NSEC_PER_MSEC);

	printf(" ---------------------------------------------------\n");
	//	print_bad_events(irq);
	printf("\n");

}
static int perf_irq__report(struct perf_irq *irq) 
{
	int i;
	setup_pager();

	if (perf_irq__read_events(irq))  
		return -1;

	if(irq->irq_soc.profile_key >= 0)
	{
		function_report_print_by_key(&irq->irq_soc, &irq->cmp_pid);
	}
	else if(irq->softirq_soc.profile_key >= 0)
	{
		function_report_print_by_key(&irq->softirq_soc, &irq->cmp_pid);
	}
	else
	{
		printf("IRQ:");
		function_report_print(&irq->sort_list, &irq->irq_soc);

		if(irq->irq_soc.profile_cpu >= 0)
		{
			for(i=0; i<=irq->irq_soc.max_cpu;i++)
			{
				if(irq->irq_soc.func_cpu[i] != NULL)
				{
					printf("CPU %d:",i);
					function_report_print(&irq->sort_list, irq->irq_soc.func_cpu[i]);
				}
			}
		}

		printf("SOFTIRQ:");
		function_report_print(&irq->sort_list, &irq->softirq_soc);
		
		if(irq->softirq_soc.profile_cpu >= 0)
		{
			for(i=0; i<=irq->softirq_soc.max_cpu;i++)
			{
				if(irq->softirq_soc.func_cpu[i] != NULL)
				{
					printf("CPU %d:",i);
					function_report_print(&irq->sort_list, irq->softirq_soc.func_cpu[i]);
				}
			}
		}
	}

	return 0;
}

static void setup_sorting(struct perf_irq *irq, const struct option *options,
			  const char * const usage_msg[])
{
	char *tmp, *tok, *str = strdup(irq->sort_order);

	for (tok = strtok_r(str, ", ", &tmp);
			tok; tok = strtok_r(NULL, ", ", &tmp)) {
		if (sort_dimension__add(tok, &irq->sort_list) < 0) {
			usage_with_options_msg(usage_msg, options,
					"Unknown --sort key: `%s'", tok);
		}
	}

	free(str);

	sort_dimension__add("irq", &irq->cmp_pid);
}

static void func_class_init(struct func_class *func_class, int profile_cpu, int profile_key)
{
	int i;
	for(i=0;i<MAX_CPUS;i++)
	{
		func_class->func_cpu[i] = NULL;
	}
	func_class->profile_cpu = profile_cpu;
	func_class->profile_key = profile_key;
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
		"-e", "irq:irq_handler_entry",
		"-e", "irq:irq_handler_exit",
		"-e", "irq:softirq_entry",
		"-e", "irq:softirq_exit",
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

int cmd_irq(int argc, const char **argv)
{
	const char default_sort_order[] = "irq, avg, max, min, count, runtime";
	struct perf_irq irq = {
		.tool = {
			.sample		 = perf_irq__process_tracepoint_sample,
			.comm		 = perf_event__process_comm,
			.namespaces	 = perf_event__process_namespaces,
			.lost		 = perf_event__process_lost,
			.ordered_events = true,
		},
		.cmp_pid	      = LIST_HEAD_INIT(irq.cmp_pid),
		.sort_list	      = LIST_HEAD_INIT(irq.sort_list),
		.sort_order	      = default_sort_order,
		.profile_cpu	      = -1,
		.profile_irq	      = -1,
		.profile_softirq		  = -1,

	};
	const struct option irq_options[] = {
	OPT_STRING('i', "input", &input_name, "file",
		    "input file name"),
	OPT_INCR('v', "verbose", &verbose,
		    "be more verbose (show symbol address, etc)"),
	OPT_BOOLEAN('D', "dump-raw-trace", &dump_trace,
		    "dump raw trace in ASCII"),
	OPT_BOOLEAN('f', "force", &irq.force, "don't complain, do it"),
	OPT_END()
	};
	const struct option report_options[] = {
	OPT_STRING('s', "sort", &irq.sort_order, "key[,key2...]",
		   "sort by key(s): irq, avg, max, min, count, runtime"),
	OPT_INTEGER('C', "CPU", &irq.profile_cpu,
		    "CPU to profile on"),
	OPT_INTEGER('H', "irq", &irq.profile_irq,
		    "set irq to profile"),
	OPT_INTEGER('S', "softirq", &irq.profile_softirq,
		    "set softirq to profile"),
	OPT_PARENT(irq_options)
	};

	const char * const report_usage[] = {
		"perf sched latency [<options>]",
		NULL
	};

	const char *const irq_subcommands[] = { "record", "report", NULL };
	const char *irq_usage[] = {
		NULL,
		NULL
	};

	argc = parse_options_subcommand(argc, argv, irq_options, irq_subcommands,
					irq_usage, PARSE_OPT_STOP_AT_NON_OPTION);
	if (!argc)
		usage_with_options(irq_usage, irq_options);

	if (!strncmp(argv[0], "rec", 3)) {
		return __cmd_record(argc, argv);
	} else if (!strncmp(argv[0], "rep", 3)) {
		if (argc > 1) {
			argc = parse_options(argc, argv, report_options, report_usage, 0);
			if (argc)
				usage_with_options(report_usage, report_options);
		}
		func_class_init(&irq.irq_soc, irq.profile_cpu, irq.profile_irq);
		func_class_init(&irq.softirq_soc, irq.profile_cpu, irq.profile_softirq);
		setup_sorting(&irq, report_options, report_usage);
		return perf_irq__report(&irq);
	} else {
		usage_with_options(irq_usage, irq_options);
	}

	return 0;
}
