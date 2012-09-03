#include "redis.h"

/* 在 redis.h 中和事务/WATCH有关的结构
 * 
 * typedef struct redisClient {
 *   // ...
 *   // 当前 DB
 *   redisDb *db;                
 *   // multi 的状态
 *   multiState mstate;          
 *   // 这个客户端 WATCH 的所有 KEY
 *   list *watched_keys;         
 *   // ...
 * } redisClient;
 *
 * typedef struct multiState {
 *   // 保存事务中所有命令的数组（FIFO 形式）
 *   multiCmd *commands;         
 *   // 命令的数量
 *   int count;                  
 * } multiState;
 *
 * typedef struct multiCmd {
 *   // 命令参数
 *   robj **argv;                
 *   // 命令参数数量
 *   int argc;                   
 *   // 命令
 *   struct redisCommand *cmd;   
 * } multiCmd;
 *
 * typedef struct redisDb {
 *    // ...
 *    // 链表，记录了所有被 watche 的 key
 *    dict *watched_keys;   
 *    // ...
 * } redisDb;
 *
 */

/* ================================ MULTI/EXEC ============================== */

/* Client state initialization for MULTI/EXEC */
// 初始化客户端状态，为执行事务做好准备
void initClientMultiState(redisClient *c) {
    // 清空命令数组
    c->mstate.commands = NULL;
    // 情况命令计数器
    c->mstate.count = 0;
}

/* Release all the resources associated with MULTI/EXEC state */
// 释放所有事务相关资源
void freeClientMultiState(redisClient *c) {
    int j;

    // 释放所有命令
    for (j = 0; j < c->mstate.count; j++) {
        int i;

        // 指向要被清除的命令
        multiCmd *mc = c->mstate.commands+j;

        // 释放命令参数
        for (i = 0; i < mc->argc; i++)
            decrRefCount(mc->argv[i]);
        // 释放数组
        zfree(mc->argv);
    }

    // 释放 mstate 结构
    zfree(c->mstate.commands);
}

/* Add a new command into the MULTI commands queue */
// 将新命令放入 FIFO 队列中，等待执行
void queueMultiCommand(redisClient *c) {
    multiCmd *mc;
    int j;

    // 为命令数组分配更多空间
    c->mstate.commands = zrealloc(c->mstate.commands,
            sizeof(multiCmd)*(c->mstate.count+1));
    
    // 将指针指向新命令
    mc = c->mstate.commands+c->mstate.count;
    // 设置命令
    mc->cmd = c->cmd;
    // 设置参数数量
    mc->argc = c->argc;
    // 设置参数
    mc->argv = zmalloc(sizeof(robj*)*c->argc);
    memcpy(mc->argv,c->argv,sizeof(robj*)*c->argc);
    for (j = 0; j < c->argc; j++)
        incrRefCount(mc->argv[j]);

    // 更新 FIFO 队列计数器
    c->mstate.count++;
}

// 取消事务的执行
void discardTransaction(redisClient *c) {
    // 释放事务资源 
    freeClientMultiState(c);
    // 重置事务状态
    initClientMultiState(c);
    // 关闭 FLAG
    c->flags &= ~(REDIS_MULTI|REDIS_DIRTY_CAS);;
    // 取消客户端对所有 key 的 watch
    unwatchAllKeys(c);
}

// 打开 FLAG ，标识事务的开始
void multiCommand(redisClient *c) {
    // 不可以嵌套使用 MULTI
    if (c->flags & REDIS_MULTI) {
        addReplyError(c,"MULTI calls can not be nested");
        return;
    }

    // 打开 REDIS_MULTI 设置项
    c->flags |= REDIS_MULTI;

    addReply(c,shared.ok);
}

// 取消事务
void discardCommand(redisClient *c) {
    if (!(c->flags & REDIS_MULTI)) {
        addReplyError(c,"DISCARD without MULTI");
        return;
    }
    discardTransaction(c);
    addReply(c,shared.ok);
}

/* Send a MULTI command to all the slaves and AOF file. Check the execCommand
 * implememntation for more information. */
// 向所有附属节点和 AOF 文件发送 MULTI 命令
// 保证一致性
void execCommandReplicateMulti(redisClient *c) {
    // 手动生成 MULTI 命令
    robj *multistring = createStringObject("MULTI",5);

    // 如果处于 AOF 模式，则发送 MULTI 命令到 AOF 
    if (server.aof_state != REDIS_AOF_OFF)
        feedAppendOnlyFile(server.multiCommand,c->db->id,&multistring,1);

    // 如果处于复制模式，向附属节点发送 MULTI 命令
    if (listLength(server.slaves))
        replicationFeedSlaves(server.slaves,c->db->id,&multistring,1);

    decrRefCount(multistring);
}

// 执行事务
void execCommand(redisClient *c) {
    int j;
    robj **orig_argv;
    int orig_argc;
    struct redisCommand *orig_cmd;

    if (!(c->flags & REDIS_MULTI)) {
        addReplyError(c,"EXEC without MULTI");
        return;
    }

    /* Check if we need to abort the EXEC if some WATCHed key was touched.
     * A failed EXEC will return a multi bulk nil object. */
    // 如果在执行事务之前，有监视中（WATCHED）的 key 被改变
    // 那么取消这个事务
    if (c->flags & REDIS_DIRTY_CAS) {
        freeClientMultiState(c);
        initClientMultiState(c);
        c->flags &= ~(REDIS_MULTI|REDIS_DIRTY_CAS);
        unwatchAllKeys(c);
        addReply(c,shared.nullmultibulk);
        return;
    }

    /* Replicate a MULTI request now that we are sure the block is executed.
     * This way we'll deliver the MULTI/..../EXEC block as a whole and
     * both the AOF and the replication link will have the same consistency
     * and atomicity guarantees. */
    // 为保证一致性和原子性
    // 如果处在 AOF 模式中，向 AOF 文件发送 MULTI 
    // 如果处在复制模式中，向附属节点发送 MULTI
    execCommandReplicateMulti(c);

    /* Exec all the queued commands */
    // 开始执行所有事务中的命令（FIFO 方式）
    unwatchAllKeys(c); /* Unwatch ASAP otherwise we'll waste CPU cycles */

    // 备份所有参数和命令
    orig_argv = c->argv;
    orig_argc = c->argc;
    orig_cmd = c->cmd;
    // 设置回复长度
    addReplyMultiBulkLen(c,c->mstate.count);
    for (j = 0; j < c->mstate.count; j++) {
        // 取出命令、参数和参数长度
        c->argc = c->mstate.commands[j].argc;
        c->argv = c->mstate.commands[j].argv;
        c->cmd = c->mstate.commands[j].cmd;

        // 执行命令
        call(c,REDIS_CALL_FULL);

        /* Commands may alter argc/argv, restore mstate. */
        c->mstate.commands[j].argc = c->argc;
        c->mstate.commands[j].argv = c->argv;
        c->mstate.commands[j].cmd = c->cmd;
    }

    // 恢复所有参数和命令
    c->argv = orig_argv;
    c->argc = orig_argc;
    c->cmd = orig_cmd;

    // 重置事务状态
    freeClientMultiState(c);
    initClientMultiState(c);
    c->flags &= ~(REDIS_MULTI|REDIS_DIRTY_CAS);

    /* Make sure the EXEC command is always replicated / AOF, since we
     * always send the MULTI command (we can't know beforehand if the
     * next operations will contain at least a modification to the DB). */
    // 更新状态值，确保事务执行之后的状态为脏
    server.dirty++;
}

/* ===================== WATCH (CAS alike for MULTI/EXEC) ===================
 *
 * The implementation uses a per-DB hash table mapping keys to list of clients
 * WATCHing those keys, so that given a key that is going to be modified
 * we can mark all the associated clients as dirty.
 *
 * Also every client contains a list of WATCHed keys so that's possible to
 * un-watch such keys when the client is freed or when UNWATCH is called. */
// 实现：
// 为每个 DB 保存一个哈希表
// 哈希表的键是被 WATCH 的 KEY ，值是一个列表，列表中保存了所有 WATCH 这个 KEY 的客户端
// 这样每当某个 KEY 被修改了，那么所有 WATCH 它的客户端都会被标记为 dirty
//
// 另外每个客户端也保存一个被 WATCH KEY 的链表，
// 这样就可以在事务执行完毕或者执行 UNWATCH 命令的时候
// 一次性对客户端 WATCHED 的所有 KEY 进行 UNWATCH


/* In the client->watched_keys list we need to use watchedKey structures
 * as in order to identify a key in Redis we need both the key name and the
 * DB */
// 每个被 WATCH 的 KEY 都会根据 KEY 名和 DB 
// 被保存到 redisClient.watched_keys 这个链表中
typedef struct watchedKey {
    // 被监视的 key
    robj *key;
    // 该 key 所在的 db
    // 注意，这里使用指向 redisDb 的指针
    // 而不是单纯的 db number
    // 因为操作函数需要使用这个 redisDb 实例
    redisDb *db;
} watchedKey;

/* Watch for the specified key */
// WATCH 某个 KEY
void watchForKey(redisClient *c, robj *key) {
    list *clients = NULL;
    listIter li;
    listNode *ln;
    watchedKey *wk;

    /* Check if we are already watching for this key */
    // 所有被 WATCHED 的 KEY 都被放在 redisClient.watched_keys 链表中
    // 遍历这个链表，查看这个 KEY 是否已经处于监视状态（WATCHED）
    listRewind(c->watched_keys,&li);
    while((ln = listNext(&li))) {
        wk = listNodeValue(ln);
        if (wk->db == c->db && equalStringObjects(key,wk->key))
            return; /* Key already watched */
    }

    /* This key is not already watched in this DB. Let's add it */
    // 如果 KEY 还没有被 WATCH 过，那么对它进行 WATCH
    clients = dictFetchValue(c->db->watched_keys,key);
    if (!clients) { 
        // 如果 clients 链表不存在
        // 说明这个客户端是第一个监视这个 DB 的这个 KEY 的客户端
        // 那么 clients 创建链表，并将它添加到 c->db->watched_keys 字典中
        clients = listCreate();
        dictAdd(c->db->watched_keys,key,clients);
        incrRefCount(key);
    }
    // 否则，将客户端添加到 clients 链表
    listAddNodeTail(clients,c);

    /* Add the new key to the lits of keys watched by this client */
    // 除了 c->db->watched_keys 之外
    // 还要将被 WATCH 的 KEY 添加到 c->watched_keys
    wk = zmalloc(sizeof(*wk));
    wk->key = key;
    wk->db = c->db;
    incrRefCount(key);
    listAddNodeTail(c->watched_keys,wk);
}

/* Unwatch all the keys watched by this client. To clean the EXEC dirty
 * flag is up to the caller. */
// 撤销对这个客户端的所有 WATCH
// 清除 EXEC dirty FLAG 的任务由调用者完成
void unwatchAllKeys(redisClient *c) {
    listIter li;
    listNode *ln;

    // 没有 WATCHED KEY ，直接返回
    if (listLength(c->watched_keys) == 0) return;

    // 用迭代器遍历客户端所有监视中的 key
    listRewind(c->watched_keys,&li);
    while((ln = listNext(&li))) {
        list *clients;
        watchedKey *wk;

        /* Lookup the watched key -> clients list and remove the client
         * from the list */
        // 将当前客户端从监视 KEY 的链表中移除
        wk = listNodeValue(ln);
        clients = dictFetchValue(wk->db->watched_keys, wk->key);
        redisAssertWithInfo(c,NULL,clients != NULL);
        listDelNode(clients,listSearchKey(clients,c));

        /* Kill the entry at all if this was the only client */
        // 如果监视 KEY 的只有这个客户端
        // 那么将链表从字典中删除
        if (listLength(clients) == 0)
            dictDelete(wk->db->watched_keys, wk->key);
        /* Remove this watched key from the client->watched list */
        // 还需要将 KEY 从 client->watched_keys 链表中移除
        listDelNode(c->watched_keys,ln);
        decrRefCount(wk->key);
        zfree(wk);
    }
}

/* "Touch" a key, so that if this key is being WATCHed by some client the
 * next EXEC will fail. */
// 打开所有 WATCH 给定 KEY 的客户端的 REDIS_DIRTY_CAS 状态
// 使得接下来的 EXEC 执行失败
void touchWatchedKey(redisDb *db, robj *key) {
    list *clients;
    listIter li;
    listNode *ln;

    // db 没有监视中的 key
    if (dictSize(db->watched_keys) == 0) return;

    // 查找所有正在监视 key 的客户端
    clients = dictFetchValue(db->watched_keys, key);

    // 没有客户端在监视 key
    if (!clients) return;

    /* Mark all the clients watching this key as REDIS_DIRTY_CAS */
    /* Check if we are already watching for this key */
    // 将所有监视 key 的客户端的状态都设置为脏
    listRewind(clients,&li);
    while((ln = listNext(&li))) {
        redisClient *c = listNodeValue(ln);

        c->flags |= REDIS_DIRTY_CAS;
    }
}

/* On FLUSHDB or FLUSHALL all the watched keys that are present before the
 * flush but will be deleted as effect of the flushing operation should
 * be touched. "dbid" is the DB that's getting the flush. -1 if it is
 * a FLUSHALL operation (all the DBs flushed). */
// 在 FLUSHDB 或者 FLUSHALL 命令执行时
// 将监视这个 db 的所有客户端的 REDIS_DIRTY_CAS 状态打开
// dbid 变量指示要被 FLUSH 的 DB 号码
// 如果 dbid 为 -1 ，表示所有 DB 都要被 FLUSH（也即是 FLUSHALL）
void touchWatchedKeysOnFlush(int dbid) {
    listIter li1, li2;
    listNode *ln;

    /* For every client, check all the waited keys */
    // 遍历所有客户端
    listRewind(server.clients,&li1);
    while((ln = listNext(&li1))) {
        redisClient *c = listNodeValue(ln);
        // 遍历客户端监视的所有 key
        listRewind(c->watched_keys,&li2);
        while((ln = listNext(&li2))) {
            watchedKey *wk = listNodeValue(ln);

            /* For every watched key matching the specified DB, if the
             * key exists, mark the client as dirty, as the key will be
             * removed. */
            // FLUSHALL ？
            if (dbid == -1 || wk->db->id == dbid) {
                // 有正在 watche 的 key ？
                if (dictFind(wk->db->dict, wk->key->ptr) != NULL)
                    c->flags |= REDIS_DIRTY_CAS;
            }
        }
    }
}

void watchCommand(redisClient *c) {
    int j;

    if (c->flags & REDIS_MULTI) {
        addReplyError(c,"WATCH inside MULTI is not allowed");
        return;
    }
    for (j = 1; j < c->argc; j++)
        watchForKey(c,c->argv[j]);
    addReply(c,shared.ok);
}

void unwatchCommand(redisClient *c) {
    unwatchAllKeys(c);
    // 关闭 REDIS_DIRTY_CAS 项
    c->flags &= (~REDIS_DIRTY_CAS);
    addReply(c,shared.ok);
}
