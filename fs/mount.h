#include <linux/mount.h>
#include <linux/seq_file.h>
#include <linux/poll.h>

struct mnt_namespace {
	atomic_t		count;
	unsigned int		proc_inum;
	struct mount *	root;
	struct list_head	list;
	struct user_namespace	*user_ns;
	u64			seq;	/* Sequence number to prevent loops */
	wait_queue_head_t poll;
	int event;
};

struct mnt_pcp {
	int mnt_count;
	int mnt_writers;
};
/*挂载点，每挂载一次会创建一个。同一个路径上可能挂载多个文件系统。最后一次的会覆盖前面的*/
struct mountpoint {
	struct list_head m_hash;
	struct dentry *m_dentry;
	int m_count;
};
/*表示一个挂载点的挂载信息。每次挂载文件系统都会创建一个*/
struct mount {
	struct list_head mnt_hash;
	struct mount *mnt_parent;       /*指向父目录所属的mount*/
	struct dentry *mnt_mountpoint;  /*指向所挂载的父目录的dentry*/
	struct vfsmount mnt;            /*本挂载点的mnt*/
#ifdef CONFIG_SMP
	struct mnt_pcp __percpu *mnt_pcp;
#else
	int mnt_count;
	int mnt_writers;
#endif
	struct list_head mnt_mounts;	/* list of children, anchored here */  /*指向mnt下的所有子挂载点*/
	struct list_head mnt_child;	/* and going through their mnt_child */    /*链接到父节点的mnt_mounts上*/
	struct list_head mnt_instance;	/* mount instance on sb->s_mounts */
	const char *mnt_devname;	/* Name of device e.g. /dev/dsk/hda1 */
	struct list_head mnt_list;  /*链接本目录下的所有mnt*/
	struct list_head mnt_expire;	/* link in fs-specific expiry list */  /*到期链表*/
	struct list_head mnt_share;	/* circular list of shared mounts */       /*共享装载的循环链表*/
	struct list_head mnt_slave_list;/* list of slave mounts */             /*从属装载的链表*/
	struct list_head mnt_slave;	/* slave list entry */
	struct mount *mnt_master;	/* slave is on master->mnt_slave_list */   /*指向主装载*/
	struct mnt_namespace *mnt_ns;	/* containing namespace */             /*所属命名空间*/
	struct mountpoint *mnt_mp;	/* where is it mounted */
#ifdef CONFIG_FSNOTIFY
	struct hlist_head mnt_fsnotify_marks;
	__u32 mnt_fsnotify_mask;
#endif
	int mnt_id;			/* mount identifier */
	int mnt_group_id;		/* peer group identifier */
	int mnt_expiry_mark;		/* true if marked for expiry */    /*如果标记为到期其值为true*/
	int mnt_pinned;
	int mnt_ghosts;
};

#define MNT_NS_INTERNAL ERR_PTR(-EINVAL) /* distinct from any mnt_namespace */

static inline struct mount *real_mount(struct vfsmount *mnt)
{
	return container_of(mnt, struct mount, mnt);
}

static inline int mnt_has_parent(struct mount *mnt)
{
	return mnt != mnt->mnt_parent;
}

static inline int is_mounted(struct vfsmount *mnt)
{
	/* neither detached nor internal? */
	return !IS_ERR_OR_NULL(real_mount(mnt));
}

extern struct mount *__lookup_mnt(struct vfsmount *, struct dentry *, int);

static inline void get_mnt_ns(struct mnt_namespace *ns)
{
	atomic_inc(&ns->count);
}

struct proc_mounts {
	struct seq_file m;
	struct mnt_namespace *ns;
	struct path root;
	int (*show)(struct seq_file *, struct vfsmount *);
};

#define proc_mounts(p) (container_of((p), struct proc_mounts, m))

extern const struct seq_operations mounts_op;
