#include "newfs.h"

extern struct custom_options newfs_options;			 /* 来自newfs的 全局选项 */
extern struct newfs_super newfs_super; 


/**
 * 磁盘交互的封装 
 * newfs_driver_read 封装 ddriver_read
 * newfs_driver_write 封装 ddriver_write
 * ddriver_read/write 读取或写入一个IO块大小的数据
 * ddriver_seek来移动要读取或写入的起始位置，也就是磁盘头。
 * 
 * 为了能够更加灵活往磁盘任何一个位置offset读写任意大小size的数据：
 * 大概思路是先把数据所在的磁盘块都读出来，
 * 然后再从这一部分读出的数据中读写相应的数据，若是写，则要把读入修改的部分再写回磁盘。
*/
// 是NEWFS_IO_SZ好呢，还是NEWFS_BLK_SZ好呢  暂且不动 用NEWFS_IO_SZ
/**
 * @brief 驱动读
 * 
 * @param offset 
 * @param out_content 
 * @param size 
 * @return int 
 */
int newfs_driver_read(int offset, uint8_t *out_content, int size) {
    // 据传入的offset和size，确定要读取的数据段和512B对齐的下界down和上界up
    int      offset_aligned = NEWFS_ROUND_DOWN(offset, NEWFS_IO_SZ());
    int      bias           = offset - offset_aligned;
    int      size_aligned   = NEWFS_ROUND_UP((size + bias), NEWFS_IO_SZ());
    uint8_t* temp_content   = (uint8_t*)malloc(size_aligned);
    uint8_t* cur            = temp_content;
    //利用ddriver_seek移动把磁盘头到down位置，
    //然后用ddriver_read读取一个磁盘块，
    //再移动磁盘头读取下一个磁盘块，
    //最后将从down到up的磁盘块都读取到内存中。
    ddriver_seek(NEWFS_DRIVER(), offset_aligned, SEEK_SET);
    while (size_aligned != 0)
    {
        ddriver_read(NEWFS_DRIVER(), cur, NEWFS_IO_SZ());
        cur          += NEWFS_IO_SZ();
        size_aligned -= NEWFS_IO_SZ();   
    }
    // 然后拷贝所需要的部分，从bias处开始，大小为size，进行返回。
    memcpy(out_content, temp_content + bias, size);
    free(temp_content);
    return NEWFS_ERROR_NONE;
}
/**
 * @brief 驱动写
 *  读-修改-写 三个阶段
 * @param offset 
 * @param in_content 
 * @param size 
 * @return int 
 */
int newfs_driver_write(int offset, uint8_t *in_content, int size) {
    int      offset_aligned = NEWFS_ROUND_DOWN(offset, NEWFS_IO_SZ());
    int      bias           = offset - offset_aligned;
    int      size_aligned   = NEWFS_ROUND_UP((size + bias), NEWFS_IO_SZ());
    uint8_t* temp_content   = (uint8_t*)malloc(size_aligned);
    uint8_t* cur            = temp_content;
    //先读出需要的磁盘块到内存
    newfs_driver_read(offset_aligned, temp_content, size_aligned);
    //然后在内存覆盖指定内容
    memcpy(temp_content + bias, in_content, size);
    
    ddriver_seek(NEWFS_DRIVER(), offset_aligned, SEEK_SET);
    while (size_aligned != 0)
    {
        // 然后将读出的磁盘块再依次写回到内存。
        ddriver_write(NEWFS_DRIVER(), cur, NEWFS_IO_SZ());
        cur          += NEWFS_IO_SZ();
        size_aligned -= NEWFS_IO_SZ();   
    }

    free(temp_content);
    return NEWFS_ERROR_NONE;
}

/**
 * @brief 挂载 newfs, Layout 如下
 * 
 * Layout
 * | Super | Inode Map | Data Map | Data |
 *   超级块   索引节点位图  数据块位图 索引节点 数据块
 * BLK_SZ = IO_SZ * 2   一个逻辑块是两个IO块大小
 * 
 * 挂载本质：初始化管理区缓存
 * 每个Inode占用一个Blk
 * @param options 
 * @return int 
 */
int newfs_mount(struct custom_options options){
     /*定义磁盘各部分结构*/
    int                 ret = NEWFS_ERROR_NONE;
    int                 driver_fd;
    struct newfs_super_d  newfs_super_d; 
    struct newfs_dentry*  root_dentry;
    struct newfs_inode*   root_inode;

    /*索引节点位图*/
    int                 inode_num;
    int                 map_inode_blks;
    
    /*数据块位图*/
    int                 data_num;
    int                 map_data_blks;

    int                 super_blks;
    boolean             is_init = FALSE;

    newfs_super.is_mounted = FALSE;

    // 打开驱动
    driver_fd = ddriver_open(options.device);

    if (driver_fd < 0) {// 打开驱动失败
        return driver_fd;
    }

    // 向内存超级块中标记驱动并写入磁盘大小和单次IO大小
    newfs_super.driver_fd = driver_fd;
    ddriver_ioctl(NEWFS_DRIVER(), IOC_REQ_DEVICE_SIZE,  &newfs_super.sz_disk);
    ddriver_ioctl(NEWFS_DRIVER(), IOC_REQ_DEVICE_IO_SZ, &newfs_super.sz_io);
    newfs_super.sz_blk = newfs_super.sz_io * 2; //BLK_SZ = IO_SZ * 2   一个逻辑块是两个IO块大小


    // 创建根目录项并读取磁盘超级块到内存
    root_dentry = new_dentry("/", NEWFS_DIR);

    if (newfs_driver_read(NEWFS_SUPER_OFS, (uint8_t *)(&newfs_super_d), 
                        sizeof(struct newfs_super_d)) != NEWFS_ERROR_NONE) {
        return -NEWFS_ERROR_IO;
    }   

    // 根据超级块幻数判断是否为第一次启动磁盘，如果是第一次启动磁盘，则需要建立磁盘超级块的布局。   
    /* 读取super */  /*1. 读入超级块判断是否已初始化*/
    if (newfs_super_d.magic_num != NEWFS_MAGIC) {     /* 第一次挂载  幻数无 */
        /* 估算各部分大小 */  /*//重新估算磁盘布局信息*/
        //super_blks = NEWFS_ROUND_UP(sizeof(struct newfs_super_d), NEWFS_BLKS_SZ()) / NEWFS_BLKS_SZ();
        super_blks = 1;

        map_inode_blks = 1;
        map_data_blks = 1;
        //int temp_inode_num  = ((NEWFS_DISK_SZ()/NEWFS_BLK_SZ()))/(NEWFS_DATA_PER_FILE + NEWFS_INODE_PER_FILE);
        // 4MB / 7KB = 585
        //map_inode_blks = (temp_inode_num / (NEWFS_BLKS_SZ()*8))+1;
        //map_data_blks = ((NEWFS_DISK_SZ()/NEWFS_BLK_SZ()) / (NEWFS_BLKS_SZ()*8))+1;

        //最多文件数
        //inode_num  =  NEWFS_DISK_SZ() / ((NEWFS_DATA_PER_FILE + NEWFS_INODE_PER_FILE) * NEWFS_IO_SZ());
        inode_num  = ((NEWFS_DISK_SZ()/NEWFS_BLK_SZ()) - super_blks - map_data_blks - map_inode_blks)
        /(NEWFS_DATA_PER_FILE + NEWFS_INODE_PER_FILE);
        //NEWFS_DATA_PER_FILE 每个文件的数据所占大小6  NEWFS_INODE_PER_FILE 每个文件的索引所占大小1
        data_num = inode_num*6;

        printf("data_num=%d; inode_num:%d\n",data_num,inode_num);

        /* 布局layout */  // 添加data位图 其余向后延
        newfs_super.max_ino = inode_num; 
        newfs_super.max_data = data_num;
        newfs_super_d.magic_num = NEWFS_MAGIC; //？？？？？????

        // NEWFS_BLKS_SZ(super_blks) 块的大小*块数
        newfs_super_d.map_inode_offset = NEWFS_SUPER_OFS + NEWFS_BLKS_SZ(super_blks);
        newfs_super_d.map_data_offset = newfs_super_d.map_inode_offset + NEWFS_BLKS_SZ(map_inode_blks);

        newfs_super_d.inode_offset = newfs_super_d.map_data_offset + NEWFS_BLKS_SZ(map_data_blks);
        newfs_super_d.data_offset = newfs_super_d.inode_offset + NEWFS_BLKS_SZ(inode_num);
        // 这里是否有问题           newfs_super_d.data_offset = newfs_super_d.map_inode_offset + NEWFS_BLKS_SZ(inode_num);？？？???

        newfs_super_d.map_inode_blks  = map_inode_blks;
        newfs_super_d.map_data_blks  = map_data_blks;
        newfs_super_d.max_ino = inode_num;
        newfs_super_d.max_data = data_num;

        newfs_super_d.sz_usage    = 0;

        NEWFS_DBG("inode map blocks: %d\n", map_inode_blks);
        is_init = TRUE;
    }
     /*2. 生成超级块 内存*/
    //初始化内存中的超级块，和根目录项
    // //超级块未初始化：清零索引节点 数据块位图  or  已初始化：直接读取填充磁盘布局信息
    newfs_super.sz_usage   = newfs_super_d.sz_usage;      /* 建立 in-memory 结构 */
    
    /*inode位图 相关 初始化*/
    newfs_super.map_inode = (uint8_t *)malloc(NEWFS_BLKS_SZ(newfs_super_d.map_inode_blks));
    newfs_super.map_inode_blks = newfs_super_d.map_inode_blks;
    newfs_super.map_inode_offset = newfs_super_d.map_inode_offset;
    newfs_super.inode_offset = newfs_super_d.inode_offset;
    /*data数据位图 相关 初始化*/
    newfs_super.map_data = (uint8_t *)malloc(NEWFS_BLKS_SZ(newfs_super_d.map_data_blks));
    newfs_super.map_data_blks = newfs_super_d.map_data_blks;
    newfs_super.map_data_offset = newfs_super_d.map_data_offset;
    newfs_super.data_offset = newfs_super_d.data_offset;

    /*3. 生成数据块/索引节点位图 内存*/
    // 读取inode位图   //读取索引节点 数据块位图
    if (newfs_driver_read(newfs_super_d.map_inode_offset, (uint8_t *)(newfs_super.map_inode), 
                        NEWFS_BLKS_SZ(newfs_super_d.map_inode_blks)) != NEWFS_ERROR_NONE) {
        return -NEWFS_ERROR_IO;
    }

    // 读取data数据位图
    if (newfs_driver_read(newfs_super_d.map_data_offset, (uint8_t *)(newfs_super.map_data), 
                        NEWFS_BLKS_SZ(newfs_super_d.map_data_blks)) != NEWFS_ERROR_NONE) {
        return -NEWFS_ERROR_IO;
    }

    /*4. 初始化根目录的结构，作为后续路径解析入口*/
    //创建空根目录inode及dentry
    if (is_init) {     //如果是新初始化的                               /* 分配根节点 */
        root_inode = newfs_alloc_inode(root_dentry);
        newfs_sync_inode(root_inode);
    }
    //读取根目录inode，生成层级
    root_inode            = newfs_read_inode(root_dentry, NEWFS_ROOT_INO);
    root_dentry->inode    = root_inode;
    newfs_super.root_dentry = root_dentry;
    newfs_super.is_mounted  = TRUE;

    newfs_dump_map();//这是干什么的  该有吗？？？？?????
    return ret;
}


/**
 * @brief 分配一个inode，占用位图
 * 为目录项创建inode节点
 * @param dentry 该dentry指向分配的inode
 * @return newfs_inode
 */
struct newfs_inode* newfs_alloc_inode(struct newfs_dentry * dentry) {
    printf("HAHAHAHHAHAHAH");
    struct newfs_inode* inode;
    int byte_cursor = 0;    //字节游标，用于遍历位图的字节
    int bit_cursor  = 0;    //位游标，用于遍历位图的位
    int ino_cursor  = 0;    //inode游标，表示当前处理的inode块号
    int dat_cursor  = 0;    //data游标，表示当前处理的data块号
    int dat_blks_num = 0;// 已找到空闲的data的块数 需满6个
    int dat_blks[NEWFS_DATA_PER_FILE];// 存放空闲的data块号 需找满6个才能放一个文件  

    boolean is_find_free_entry = FALSE; //是否找到未使用的inode节点
    boolean is_find_free_entry_enough_data = FALSE; //是否找到足够6个的未使用的data节点

    //​ ①在inode位图上寻找未使用的inode节点。
    for (byte_cursor = 0; byte_cursor < NEWFS_BLKS_SZ(newfs_super.map_inode_blks); 
         byte_cursor++)
    {
        for (bit_cursor = 0; bit_cursor < UINT8_BITS; bit_cursor++) {
            if((newfs_super.map_inode[byte_cursor] & (0x1 << bit_cursor)) == 0) {    
                /* 当前ino_cursor位置空闲 */
                newfs_super.map_inode[byte_cursor] |= (0x1 << bit_cursor);
                is_find_free_entry = TRUE;           
                break;
            }
            ino_cursor++;
        }
        if (is_find_free_entry) {
            break;
        }
    }

    // ②为目录项分配inode节点并建立他们之间的连接。
    if (!is_find_free_entry || ino_cursor == newfs_super.max_ino)
        return -NEWFS_ERROR_NOSPACE;


    

    // inode data 都有空闲，则分配inode
    inode = (struct newfs_inode*)malloc(sizeof(struct newfs_inode));
    inode->ino  = ino_cursor; 
    inode->size = 0;


    /* dentry指向/绑定该inode */
    dentry->inode = inode;
    dentry->ino   = inode->ino;
    /* inode指回dentry */
    inode->dentry = dentry;
    
    inode->dir_cnt = 0;
    inode->dentrys = NULL;


    // inode指向文件类型需要预分配数据指针 ？？？???
    if (NEWFS_IS_REG(inode)) {
        inode->data = (uint8_t *)malloc(NEWFS_BLKS_SZ(NEWFS_DATA_PER_FILE));
    }

    return inode;
}




// newfs_read_inode 函数作用是从磁盘中读取inode节点
/**
 * @brief 
 * 
 * @param dentry dentry指向ino，读取该inode
 * @param ino inode唯一编号
 * @return struct newfs_inode* 
 */
struct newfs_inode* newfs_read_inode(struct newfs_dentry * dentry, int ino) {
    struct newfs_inode* inode = (struct newfs_inode*)malloc(sizeof(struct newfs_inode));
    struct newfs_inode_d inode_d;
    struct newfs_dentry* sub_dentry;
    struct newfs_dentry_d dentry_d;
    int    dir_cnt = 0, i;
    // ①通过磁盘驱动来将磁盘中ino号的inode读入内存。
    if (newfs_driver_read(NEWFS_INO_OFS(ino), (uint8_t *)&inode_d, 
                        sizeof(struct newfs_inode_d)) != NEWFS_ERROR_NONE) {
        NEWFS_DBG("[%s] io error\n", __func__);
        return NULL;                    
    }
    inode->dir_cnt = 0;
    inode->ino = inode_d.ino;
    inode->size = inode_d.size;
    memcpy(inode->target_path, inode_d.target_path, NEWFS_MAX_FILE_NAME);
    inode->dentry = dentry;
    inode->dentrys = NULL;
    //​ ② 判断inode的文件类型，如果是目录类型则需要读取每一个目录项并建立连接。
    /*判断iNode节点的文件类型*/
    if (NEWFS_IS_DIR(inode)) {/*如果是目录的话需要将目录项建立连接*/
        dir_cnt = inode_d.dir_cnt;
        for (i = 0; i < dir_cnt; i++)
        {
            if (newfs_driver_read(NEWFS_DATA_OFS(ino) + i * sizeof(struct newfs_dentry_d), 
                                (uint8_t *)&dentry_d, 
                                sizeof(struct newfs_dentry_d)) != NEWFS_ERROR_NONE) {
                NEWFS_DBG("[%s] io error\n", __func__);
                return NULL;                    
            }
            sub_dentry = new_dentry(dentry_d.fname, dentry_d.ftype);
            sub_dentry->parent = inode->dentry;
            sub_dentry->ino    = dentry_d.ino; 
            newfs_alloc_dentry(inode, sub_dentry);
        }
    }//③如果是文件类型直接读取数据即可。
    else if (NEWFS_IS_REG(inode)) {
        inode->data = (uint8_t *)malloc(NEWFS_BLKS_SZ(NEWFS_DATA_PER_FILE));
        if (newfs_driver_read(NEWFS_DATA_OFS(ino), (uint8_t *)inode->data, 
                            NEWFS_BLKS_SZ(NEWFS_DATA_PER_FILE)) != NEWFS_ERROR_NONE) {
            NEWFS_DBG("[%s] io error\n", __func__);
            return NULL;                    
        }
    }
    return inode;
}


/**
 * @brief 将内存inode及其下方结构全部刷回磁盘
 * 
 * @param inode 
 * @return int 
 */
int newfs_sync_inode(struct newfs_inode * inode) {
    struct newfs_inode_d  inode_d;
    struct newfs_dentry*  dentry_cursor;
    struct newfs_dentry_d dentry_d;
    int ino             = inode->ino;
    inode_d.ino         = ino;
    inode_d.size        = inode->size;
    memcpy(inode_d.target_path, inode->target_path, NEWFS_MAX_FILE_NAME);
    inode_d.ftype       = inode->dentry->ftype;
    inode_d.dir_cnt     = inode->dir_cnt;
    int offset;

    // ①首先将inode写入磁盘
    if (newfs_driver_write(NEWFS_INO_OFS(ino), (uint8_t *)&inode_d, 
                     sizeof(struct newfs_inode_d)) != NEWFS_ERROR_NONE) {
        NEWFS_DBG("[%s] io error\n", __func__);
        return -NEWFS_ERROR_IO;
    }
    //​ ②判断inode文件类型。
                                                      /* Cycle 1: 写 INODE */
                                                      /* Cycle 2: 写 数据 */
    
    if (NEWFS_IS_DIR(inode)) {
        //​ ③如果是目录类型则需要首先将目录项写入磁盘，再递归刷写每一个目录项所对应的inode节点。                          
        dentry_cursor = inode->dentrys;
        offset        = NEWFS_DATA_OFS(ino);
        while (dentry_cursor != NULL)
        {
            // 写回所有子文件目录项
            memcpy(dentry_d.fname, dentry_cursor->fname, NEWFS_MAX_FILE_NAME);
            dentry_d.ftype = dentry_cursor->ftype;
            dentry_d.ino = dentry_cursor->ino;
            if (newfs_driver_write(offset, (uint8_t *)&dentry_d, 
                                 sizeof(struct newfs_dentry_d)) != NEWFS_ERROR_NONE) {
                NEWFS_DBG("[%s] io error\n", __func__);
                return -NEWFS_ERROR_IO;                     
            }
            // 获取各个目录项的inode 对子inode进行递归调用
            if (dentry_cursor->inode != NULL) {
                newfs_sync_inode(dentry_cursor->inode);
            }
            //移动dentry_cursor 到其兄弟节点
            dentry_cursor = dentry_cursor->brother;
            offset += sizeof(struct newfs_dentry_d);
        }
    }
    else if (NEWFS_IS_REG(inode)) {
        //④如果是文件类型，则将inode所指向的数据直接写入磁盘。
        if (newfs_driver_write(NEWFS_DATA_OFS(ino), inode->data, 
                             NEWFS_BLKS_SZ(NEWFS_DATA_PER_FILE)) != NEWFS_ERROR_NONE) {
            NEWFS_DBG("[%s] io error\n", __func__);
            return -NEWFS_ERROR_IO;
        }
    }
    return NEWFS_ERROR_NONE;
}

/**
 * @brief 
 * 卸载函数
 * @return int 
 */
int newfs_umount() {
    struct newfs_super_d  newfs_super_d; 

    if (!newfs_super.is_mounted) {
        return NEWFS_ERROR_NONE;
    }
    // ①从根节点递归往下刷写根节点。
    newfs_sync_inode(newfs_super.root_dentry->inode);     /* 从根节点向下刷写节点 */

    // ②将内存超级块转换为磁盘超级块并写入磁盘。                                                
    newfs_super_d.magic_num           = NEWFS_MAGIC;
    // 转换inode位图
    newfs_super_d.map_inode_blks      = newfs_super.map_inode_blks;
    newfs_super_d.map_inode_offset    = newfs_super.map_inode_offset;
    //转换data位图
    newfs_super_d.map_data_blks      = newfs_super.map_data_blks;
    newfs_super_d.map_data_offset    = newfs_super.map_data_offset;
    //转换data数据块和inode索引节点
    newfs_super_d.inode_offset         = newfs_super.inode_offset;
    newfs_super_d.data_offset         = newfs_super.data_offset;

    newfs_super_d.sz_usage            = newfs_super.sz_usage;

    if (newfs_driver_write(NEWFS_SUPER_OFS, (uint8_t *)&newfs_super_d, 
                     sizeof(struct newfs_super_d)) != NEWFS_ERROR_NONE) {
        return -NEWFS_ERROR_IO;
    }

    // ③将inode位图写入磁盘。
    if (newfs_driver_write(newfs_super_d.map_inode_offset, (uint8_t *)(newfs_super.map_inode), 
                         NEWFS_BLKS_SZ(newfs_super_d.map_inode_blks)) != NEWFS_ERROR_NONE) {
        return -NEWFS_ERROR_IO;
    }

    free(newfs_super.map_inode);

    // ③将data位图写入磁盘。
    if (newfs_driver_write(newfs_super_d.map_data_offset, (uint8_t *)(newfs_super.map_data), 
                         NEWFS_BLKS_SZ(newfs_super_d.map_data_blks)) != NEWFS_ERROR_NONE) {
        return -NEWFS_ERROR_IO;
    }

    free(newfs_super.map_data);

    // ​ ④关闭驱动。
    ddriver_close(NEWFS_DRIVER());

    return NEWFS_ERROR_NONE;
}

/*****************************
 * 创建目录和文件
 * ****************************/

/**
 * 路径解析，得到父目录的dentry
 * @brief 
 * path: /qwe/ad  total_lvl = 2,
 *      1) find /'s inode       lvl = 1
 *      2) find qwe's dentry 
 *      3) find qwe's inode     lvl = 2
 *      4) find ad's dentry
 *
 * path: /qwe     total_lvl = 1,
 *      1) find /'s inode       lvl = 1
 *      2) find qwe's dentry
 * 
 * @param path 
 * @return struct newfs_inode* 
 */
struct newfs_dentry* newfs_lookup(const char * path, boolean* is_find, boolean* is_root) {
    struct newfs_dentry* dentry_cursor = newfs_super.root_dentry;//设置一个游标(dentry_cursor)指向根目录的dentry
    struct newfs_dentry* dentry_ret = NULL;
    struct newfs_inode*  inode; 
    int   total_lvl = newfs_calc_lvl(path);// 计算路径的总层级数(total_lvl)，然后根据路径逐层查找对应的dentry。
    int   lvl = 0;
    boolean is_hit;
    char* fname = NULL;
    char* path_cpy = (char*)malloc(sizeof(path));
    *is_root = FALSE;
    strcpy(path_cpy, path);

    //如果路径是根目录("/")，则直接返回根目录的dentry
    if (total_lvl == 0) {                           /* 根目录 */
        *is_find = TRUE;
        *is_root = TRUE;
        dentry_ret = newfs_super.root_dentry;
    }

    fname = strtok(path_cpy, "/");       
    while (fname)
    {   
        lvl++;
        if (dentry_cursor->inode == NULL) {           /* Cache机制 */
            newfs_read_inode(dentry_cursor, dentry_cursor->ino);
        }
        // 找子
        inode = dentry_cursor->inode;
        //子是文件，函数会直接返回该子文件的dentry。
        if (NEWFS_IS_REG(inode) && lvl < total_lvl) {
            NEWFS_DBG("[%s] not a dir\n", __func__);
            dentry_ret = inode->dentry;
            break;
        }
        //是目录类型，则继续在该目录下查找下一级目录的dentry
        if (NEWFS_IS_DIR(inode)) {
            dentry_cursor = inode->dentrys;
            is_hit        = FALSE;
            // 函数会遍历该目录下的所有dentry，逐个与路径中的下一级目录名进行比较。
            
            while (dentry_cursor)
            {
                if (memcmp(dentry_cursor->fname, fname, strlen(fname)) == 0) {
                    is_hit = TRUE;// 目录名匹配上了，退出
                    break;
                }
                dentry_cursor = dentry_cursor->brother;
            }

           // 如果在当前目录下找不到匹配的dentry，则表示路径中的某一级目录不存在，此时函数会返回当前目录的dentry。
            if (!is_hit) {
                *is_find = FALSE;
                NEWFS_DBG("[%s] not found %s\n", __func__, fname);
                dentry_ret = inode->dentry;
                break;
            }
             //如果找到了路径中的最后一级目录的dentry，则表示路径有效，函数会返回该dentry。
            if (is_hit && lvl == total_lvl) {
                *is_find = TRUE;
                dentry_ret = dentry_cursor;
                break;
            }
        }
        fname = strtok(NULL, "/"); 
    }
    //最后，函数会确保返回的dentry中的inode已经被读取到内存中，并将其返回。
    if (dentry_ret->inode == NULL) {
        dentry_ret->inode = newfs_read_inode(dentry_ret, dentry_ret->ino);
    }
    
    return dentry_ret;
}

/**
 * @brief 计算路径的层级
 * exm: /av/c/d/f
 * -> lvl = 4
 * @param path 
 * @return int 
 */
int newfs_calc_lvl(const char * path) {
    // char* path_cpy = (char *)malloc(strlen(path));
    // strcpy(path_cpy, path);
    char* str = path;
    int   lvl = 0;
    if (strcmp(path, "/") == 0) {
        return lvl;
    }
    while (*str != NULL) {
        if (*str == '/') {
            lvl++;
        }
        str++;
    }
    return lvl;
}
/**
 * @brief 为一个inode分配dentry，采用头插法
 * 
 * @param inode 
 * @param dentry 
 * @return int 
 */
int newfs_alloc_dentry(struct newfs_inode* inode, struct newfs_dentry* dentry) {
    //只需要修改父目录inode中的指针指向新增的dentry结构，
    //新增的dentry的兄弟指针指向原来第一个子文件dentry即可。
    newfs_alloc_data(dentry);
    if (inode->dentrys == NULL) {
        inode->dentrys = dentry;
    }
    else {
        dentry->brother = inode->dentrys;
        inode->dentrys = dentry;
    }
    inode->dir_cnt++;
    return inode->dir_cnt;
}


/**
 * @brief 获取文件名
 * 
 * @param path 
 * @return char* 
 */
char* newfs_get_fname(const char* path) {
    char ch = '/';
    char *q = strrchr(path, ch) + 1;
    return q;
}

/**
 * @brief 
 * 
 * @param inode 
 * @param dir [0...]
 * @return struct newfs_dentry* 
 */
struct newfs_dentry* newfs_get_dentry(struct newfs_inode * inode, int dir) {
    struct newfs_dentry* dentry_cursor = inode->dentrys;
    int    cnt = 0;
    while (dentry_cursor)
    {
        if (dir == cnt) {
            return dentry_cursor;
        }
        cnt++;
        dentry_cursor = dentry_cursor->brother;
    }
    return NULL;
}


/**
 * @brief 分配一个inode，占用位图
 * 为目录项创建inode节点
 * @param dentry 该dentry指向分配的inode
 * @return newfs_inode
 */
struct newfs_inode* newfs_alloc_data(struct newfs_dentry * dentry) {
    struct newfs_inode* inode;
    int byte_cursor = 0;    //字节游标，用于遍历位图的字节
    int bit_cursor  = 0;    //位游标，用于遍历位图的位
    int ino_cursor  = 0;    //inode游标，表示当前处理的inode块号
    int dat_cursor  = 0;    //data游标，表示当前处理的data块号

    boolean is_find_free_entry = FALSE; //是否找到未使用的inode节点

    //​ ①在data位图上寻找未使用的data节点。
    for (byte_cursor = 0; byte_cursor < NEWFS_BLKS_SZ(newfs_super.map_data_blks); 
         byte_cursor++)
    {
        for (bit_cursor = 0; bit_cursor < UINT8_BITS; bit_cursor++) {
            if((newfs_super.map_data[byte_cursor] & (0x1 << bit_cursor)) == 0) {    
                /* 当前dat_cursor位置空闲 */
                newfs_super.map_data[byte_cursor] |= (0x1 << bit_cursor);
                is_find_free_entry = TRUE;           
                break;
            }
            dat_cursor++;
        }
        if (is_find_free_entry) {
            break;
        }
    }

    // ②为目录项分配inode节点并建立他们之间的连接。
    if (!is_find_free_entry || dat_cursor == newfs_super.max_data)
        return -NEWFS_ERROR_NOSPACE;

    // inode data 都有空闲，则分配inode
    /*inode = (struct newfs_inode*)malloc(sizeof(struct newfs_inode));
    inode->data  = dat_cursor; 
    inode->size = 0;*/


    if (NEWFS_IS_REG(inode)) {
        inode->data = (uint8_t *)malloc(NEWFS_BLKS_SZ(NEWFS_DATA_PER_FILE));
    }

    return inode;
}