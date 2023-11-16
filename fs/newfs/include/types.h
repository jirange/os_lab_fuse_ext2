#ifndef _TYPES_H_
#define _TYPES_H_

#define MAX_NAME_LEN    128     

/*******我加的************/
/******************************************************************************
* SECTION: Type def
*******************************************************************************/
typedef int          boolean;
typedef uint16_t     flag16;

typedef enum newfs_file_type {
    NEWFS_REG_FILE,
    NEWFS_DIR,
    NEWFS_SYM_LINK
} NEWFS_FILE_TYPE;
/******************************************************************************
* SECTION: Macro
*******************************************************************************/
#define TRUE                    1
#define FALSE                   0
#define UINT32_BITS             32
#define UINT8_BITS              8

//#define NEWFS_MAGIC_NUM           0x52415453  newfs.h定义了
#define NEWFS_SUPER_OFS           0
#define NEWFS_ROOT_INO            0

#define NEWFS_ERROR_NONE          0
//#define NEWFS_ERROR_ACCESS        EACCES
//#define NEWFS_ERROR_SEEK          ESPIPE     
//#define NEWFS_ERROR_ISDIR         EISDIR
#define NEWFS_ERROR_NOSPACE       ENOSPC
#define NEWFS_ERROR_EXISTS        EEXIST
#define NEWFS_ERROR_NOTFOUND      ENOENT
#define NEWFS_ERROR_UNSUPPORTED   ENXIO
#define NEWFS_ERROR_IO            EIO     /* Error Input/Output */
//#define NEWFS_ERROR_INVAL         EINVAL  /* Invalid Args */

#define NEWFS_MAX_FILE_NAME       128

#define NEWFS_INODE_PER_FILE      1
#define NEWFS_DATA_PER_FILE       6
//#define NEWFS_DEFAULT_PERM        0777  newfs.h定义了

//#define NEWFS_IOC_MAGIC           'S'
//#define NEWFS_IOC_SEEK            _IO(NEWFS_IOC_MAGIC, 0)

//#define NEWFS_FLAG_BUF_DIRTY      0x1
//#define NEWFS_FLAG_BUF_OCCUPY     0x2

/******************************************************************************
* SECTION: Macro Function
*******************************************************************************/
#define NEWFS_IO_SZ()                     (newfs_super.sz_io)
#define NEWFS_DISK_SZ()                   (newfs_super.sz_disk)
#define NEWFS_BLK_SZ()                   (newfs_super.sz_blk)

#define NEWFS_DRIVER()                    (newfs_super.driver_fd)

#define NEWFS_ROUND_DOWN(value, round)    (value % round == 0 ? value : (value / round) * round)
#define NEWFS_ROUND_UP(value, round)      (value % round == 0 ? value : (value / round + 1) * round)

#define NEWFS_BLKS_SZ(blks)               (blks * NEWFS_BLK_SZ())
#define NEWFS_ASSIGN_FNAME(pnewfs_dentry, _fname)\ 
                                        memcpy(pnewfs_dentry->fname, _fname, strlen(_fname))

#define NEWFS_INO_OFS(ino)                (newfs_super.data_offset + ino * NEWFS_BLKS_SZ((\
                                        NEWFS_INODE_PER_FILE + NEWFS_DATA_PER_FILE)))
#define NEWFS_DATA_OFS(ino)               (NEWFS_INO_OFS(ino) + NEWFS_BLKS_SZ(NEWFS_INODE_PER_FILE))

#define NEWFS_IS_DIR(pinode)              (pinode->dentry->ftype == NEWFS_DIR)
#define NEWFS_IS_REG(pinode)              (pinode->dentry->ftype == NEWFS_REG_FILE)
//#define NEWFS_IS_SYM_LINK(pinode)         (pinode->dentry->ftype == NEWFS_SYM_LINK)

/******************************************************************************
* SECTION: FS Specific Structure - In memory structure
*******************************************************************************/
struct newfs_dentry;
struct newfs_inode;
struct newfs_super;

/**********原来就有*************/
struct custom_options {
	const char*        device;
};

struct newfs_super {
    uint32_t magic;     // 有用吗？？？ ???
    int      driver_fd;     //原来：fd;  //空闲的数据块数???
    /* TODO: Define yourself */
    int                sz_io;
    int                sz_disk;
    int                sz_blk;  // 新加的 逻辑块的大小 应该为两倍的IO块大小
    int                sz_usage;
    
    int                max_ino;
    int                max_data;
    /*inode位图*/
    uint8_t*           map_inode;
    int                map_inode_blks;
    int                map_inode_offset;

    /*data 位图*/
    uint8_t*           map_data;
    int                map_data_blks;
    int                map_data_offset;
    
    /*索引节点和数据块的偏移*/
    int                inode_offset;
    int                data_offset;

    boolean            is_mounted;

    struct newfs_dentry* root_dentry;

};

struct newfs_inode {
    uint32_t ino;   // 换吗？  int ino;
    /* TODO: Define yourself */
    int                size;                          /* 文件已占用空间 */
    char               target_path[NEWFS_MAX_FILE_NAME];/* store traget path when it is a symlink */
    int                dir_cnt;
    struct newfs_dentry* dentry;                        /* 指向该inode的dentry */
    struct newfs_dentry* dentrys;                       /* 所有目录项 */
    uint8_t*           data;                           /*指向数据块的指针*/
    uint8_t *          block_pointer[NEWFS_DATA_PER_FILE];  //指向数据块 块号的指针      
};

struct newfs_dentry {
    char     fname[NEWFS_MAX_FILE_NAME];    //name[MAX_NAME_LEN];
    uint32_t ino;   // 换吗？  int ino;
    /* TODO: Define yourself */
    struct newfs_dentry* parent;                        /* 父亲Inode的dentry */
    struct newfs_dentry* brother;                       /* 兄弟 */
    
    struct newfs_inode*  inode;                         /* 指向inode */
    NEWFS_FILE_TYPE      ftype;
};

// 创建目录项
static inline struct newfs_dentry* new_dentry(char * fname, NEWFS_FILE_TYPE ftype) {
    struct newfs_dentry * dentry = (struct newfs_dentry *)malloc(sizeof(struct newfs_dentry));
    memset(dentry, 0, sizeof(struct newfs_dentry));
    NEWFS_ASSIGN_FNAME(dentry, fname);
    dentry->ftype   = ftype;
    dentry->ino     = -1;
    dentry->inode   = NULL;
    dentry->parent  = NULL;
    dentry->brother = NULL; 
    return dentry;                                           
}

/******************************************************************************
* SECTION: FS Specific Structure - Disk structure
*******************************************************************************/
struct newfs_super_d
{
    uint32_t           magic_num;
    int                sz_usage;
    
    int                max_ino;
    int                max_data;

    int                map_inode_blks;
    int                map_inode_offset;

    int                map_data_blks;
    int                map_data_offset;

    int                inode_offset;
    int                data_offset;

};

struct newfs_inode_d
{
    int                ino;                           /* 在inode位图中的下标 */
    int                size;                          /* 文件已占用空间 */
    char               target_path[NEWFS_MAX_FILE_NAME];/* store traget path when it is a symlink */
    int                dir_cnt;
    NEWFS_FILE_TYPE      ftype;   
};  

struct newfs_dentry_d
{
    char               fname[NEWFS_MAX_FILE_NAME];
    NEWFS_FILE_TYPE      ftype;
    int                ino;                           /* 指向的ino号 */
};  


#endif /* _TYPES_H_ */