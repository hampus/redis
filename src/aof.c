#include "redis.h"

#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>

void flushAppendOnlyFile(void);
void aofUpdateCurrentSize(void);
int aofStartWriteThread(void);
void aofStopWriteThread(void);
void * aofWriteThread(void*);
void aofUnblockClients(list *blocked_clients);
void aofWriteThreadHandleRewriteDone(void);

void aofInit(void) {
    if (pthread_mutex_init(&server.aof.mutex, NULL)) {
        redisLog(REDIS_WARNING, "Can't initialize mutex: %s", strerror(errno));
        exit(1);
    }
    if (pthread_cond_init(&server.aof.writecond, NULL)) {
        redisLog(REDIS_WARNING, "Can't initialize condition variable: %s", strerror(errno));
        exit(1);
    }
    if (server.appendonly) {
        server.aof.appendfd = open(server.appendfilename,O_WRONLY|O_APPEND|O_CREAT,0644);
        if (server.aof.appendfd == -1) {
            redisLog(REDIS_WARNING, "Can't open the append-only file: %s",
                strerror(errno));
            exit(1);
        }
    }
    /* Create internal AOF pipe */
    if (pipe(server.aof.pipefd)) {
        redisLog(REDIS_WARNING, "Can't create internal pipe: %s", strerror(errno));
        exit(1);
    }
    fcntl(server.aof.pipefd[0], F_SETFL, O_NONBLOCK);
    fcntl(server.aof.pipefd[1], F_SETFL, O_NONBLOCK);
    if (aeCreateFileEvent(server.el, server.aof.pipefd[0], AE_READABLE,
        aofPipeHandler, NULL) == AE_ERR) oom("creating pipe event");
}

void aofPipeHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    REDIS_NOTUSED(el);
    REDIS_NOTUSED(fd);
    REDIS_NOTUSED(privdata);
    REDIS_NOTUSED(mask);
    /* Only read a character from the pipe. We ignore the results, as we're
     * only interested in being notified. See beforeSleep() for more. */
    char buf[1];
    read(server.aof.pipefd[0], buf, 1);
}

/* Called when the user switches from "appendonly yes" to "appendonly no"
 * at runtime using the CONFIG command. */
void stopAppendOnly(void) {
    /* Stop write thread */
    aofStopWriteThread();
    /* Clean up appendfd */
    aofLock();
    if (server.aof.appendfd != -1) {
        flushAppendOnlyFile();
        aof_fsync(server.aof.appendfd);
        close(server.aof.appendfd);
        server.aof.appendfd = -1;
    }
    aofUnlock();
    /* Deactivate append only file */
    server.appendseldb = -1;
    server.appendonly = 0;
    /* Rewrite operation in progress? kill it, wait child exit */
    if (server.bgrewritechildpid != -1) {
        int statloc;

        if (kill(server.bgrewritechildpid,SIGKILL) != -1)
            wait3(&statloc,0,NULL);
        /* Clean up temp file */
        aofRemoveTempFile(server.bgrewritechildpid);
        /* Reset the buffer accumulating changes while the child saves */
        sdsfree(server.bgrewritebuf);
        server.bgrewritebuf = sdsempty();
        server.bgrewritechildpid = -1;
    }
}

/* Called when the user switches from "appendonly no" to "appendonly yes"
 * at runtime using the CONFIG command. */
int startAppendOnly(void) {
    aofLock();
    server.appendonly = 1;
    server.aof.lastfsync = time(NULL);
    server.aof.appendfd = open(server.appendfilename,O_WRONLY|O_APPEND|O_CREAT,0644);
    if (server.aof.appendfd == -1) {
        redisLog(REDIS_WARNING,"Used tried to switch on AOF via CONFIG, but I can't open the AOF file: %s",strerror(errno));
        aofUnlock();
        return REDIS_ERR;
    }
    aofUnlock();
    if (aofStartWriteThread() == REDIS_ERR) {
        redisLog(REDIS_WARNING,"Used tried to switch on AOF via CONFIG, but I can't create write thread: %s",strerror(errno));
        stopAppendOnly();
        return REDIS_ERR;
    }
    if (rewriteAppendOnlyFileBackground() == REDIS_ERR) {
        redisLog(REDIS_WARNING,"Used tried to switch on AOF via CONFIG, I can't trigger a background AOF rewrite operation. Check the above logs for more info about the error.",strerror(errno));
        stopAppendOnly();
        return REDIS_ERR;
    }
    return REDIS_OK;
}

/* Start the write thread if not already started. Returns REDIS_OK on success
 * and REDIS_ERR on failure. */
int aofStartWriteThread(void) {
    aofLock();
    if (server.aof.writestate != REDIS_AOF_WRITE_THREAD_NOTSTARTED) {
        aofUnlock();
        return REDIS_OK;
    }
    if (pthread_create(&server.aof.writethread, NULL, aofWriteThread, 0)) {
        aofUnlock();
        return REDIS_ERR;
    }
    server.aof.writestate = REDIS_AOF_WRITE_THREAD_ACTIVE;
    aofUnlock();
    return REDIS_OK;
}

/* Stop the AOF write thread, if running. */
void aofStopWriteThread(void) {
    aofLock();
    /* Abort rewrite, if in final stages and abortable */
    if (server.aof.writestate == REDIS_AOF_WRITE_THREAD_REWRITE) {
        sdsfree(server.aof.aofbuf);
        server.aof.aofbuf = sdsempty();
        server.aof.writestate = REDIS_AOF_WRITE_THREAD_ACTIVE;
        aofRemoveTempFile(server.bgrewritechildpid);
        server.bgrewritechildpid = -1;
        server.aof.bgrewritechildpid = -1;
    }
    /* Shutdown write thread, if active */
    if (server.aof.writestate == REDIS_AOF_WRITE_THREAD_ACTIVE) {
        server.aof.writestate = REDIS_AOF_WRITE_THREAD_SHUTDOWN;
        pthread_cond_signal(&server.aof.writecond);
        aofUnlock();
        pthread_join(server.aof.writethread, NULL);
        aofLock();
    }
    /* Check if a rewrite has finished recently */
    if (server.aof.bgrewrite_finished) {
        server.aof.bgrewrite_finished = 0;
        server.bgrewritechildpid = -1;
        /* Some junk may have accumulated in bgrewritebuf. Throw it away. */
        sdsfree(server.bgrewritebuf);
        server.bgrewritebuf = sdsempty();
    }
    aofUnlock();
}

/* The main loop of the AOF write thread. Runs in its own thread and
 * communicates with the main thread through server.aof, using a mutex and a
 * condition variable. See also: aofLock(), aofUnlock(). */
void * aofWriteThread(void *data) {
    REDIS_NOTUSED(data);
    aofLock();
    int running = 1;
    while(running) {
        switch(server.aof.writestate) {
        case REDIS_AOF_WRITE_THREAD_ACTIVE:
            if (sdslen(server.aof.aofbuf) > 0) {
                flushAppendOnlyFile();
            } else if (listLength(server.aof.blocked_clients)) {
                /* Nothing to write, just unblock waiters */
                aofUnblockClients(server.aof.blocked_clients);
            } else {
                /* Wake up the main thread, just to be sure */
                pthread_cond_signal(&server.aof.writecond);
                /* Wait until something happens */
                pthread_cond_wait(&server.aof.writecond, &server.aof.mutex);
            }
        break;
        case REDIS_AOF_WRITE_THREAD_REWRITE:
            aofWriteThreadHandleRewriteDone();
        break;
        default: /* REDIS_AOF_WRITE_THREAD_SHUTDOWN or unexpected state */
            running = 0;
        break;
        }
    }
    /* Shut down write thread */
    server.aof.writestate = REDIS_AOF_WRITE_THREAD_NOTSTARTED;
    aofUnlock();
    return NULL;
}

/* Locks server.aof. It should not be accessed without locking it first,
 * because it's shared between threads. The lock is not reentrant! Functions
 * expect server.aof to be unlocked when you call them and will also return it
 * unlocked (unless they say otherwise). */
void aofLock(void) {
    if (pthread_mutex_lock(&server.aof.mutex)) {
        redisLog(REDIS_WARNING,"Fatal: failed to lock AOF mutex.",strerror(errno));
        exit(1); /* We can't safely continue */
    }
}

/* Unlock server.aof again, which must be locked when you call this. */
void aofUnlock(void) {
    pthread_mutex_unlock(&server.aof.mutex);
}

/* Schedule clients that are blocked for unblocking. server.aof should be
 * locked before and after the call. */
void aofUnblockClients(list *blocked_clients) {
    /* Move them to the unblocked list */
    listIter *li = listGetIterator(blocked_clients, AL_START_HEAD);
    listNode *ln;
    redisClient *c;
    while((ln = listNext(li))) {
        c = ln->value;
        listDelNode(blocked_clients, ln);
        listAddNodeTail(server.aof.unblocked_clients, c);
    }
    listReleaseIterator(li);
    /* Signal main thread */
    char buf[] = "0"; /* Just write any character */
    write(server.aof.pipefd[1], buf, 1);
}

/* Write the append only file buffer on disk. Expects server.aof to be locked
 * when called and will keep it locked upon return. */
void flushAppendOnlyFile(void) {
    time_t now;
    ssize_t nwritten;
    
    if (sdslen(server.aof.aofbuf) == 0) return;

    int appendfd = server.aof.appendfd;
    sds aofbuf = server.aof.aofbuf;
    server.aof.aofbuf = sdsempty();
    list *blocked_clients = server.aof.blocked_clients;
    server.aof.blocked_clients = listCreate();
    server.aof.appendonly_current_size += sdslen(aofbuf);
    server.aof.last_write_buffer_size = sdslen(aofbuf);

    int appendfsync = server.aof.appendfsync;
    time_t lastfsync = server.aof.lastfsync;
    int no_appendfsync = server.aof.no_appendfsync;
    aofUnlock();

    /* Wake up the main thread, if it is blocking because of a full buffer */
    pthread_cond_signal(&server.aof.writecond);

    /* We want to perform a single write. This should be guaranteed atomic
     * at least if the filesystem we are writing is a real physical one.
     * While this will save us against the server being killed I don't think
     * there is much to do about the whole server stopping for power problems
     * or alike */
     nwritten = write(appendfd,aofbuf,sdslen(aofbuf));
     if (nwritten != (signed)sdslen(aofbuf)) {
        /* Ooops, we are in troubles. The best thing to do for now is
         * aborting instead of giving the illusion that everything is
         * working as expected. */
         if (nwritten == -1) {
            redisLog(REDIS_WARNING,"Exiting on error writing to the append-only file: %s",strerror(errno));
         } else {
            redisLog(REDIS_WARNING,"Exiting on short write while writing to the append-only file: %s",strerror(errno));
         }
         exit(1);
    }
    sdsfree(aofbuf);

    /* Fsync if needed */
    now = time(NULL);
    int fsync_needed = (appendfsync == APPENDFSYNC_ALWAYS || 
            (appendfsync == APPENDFSYNC_EVERYSEC && now - lastfsync > 1));
    if (fsync_needed && !no_appendfsync) {
        /* aof_fsync is defined as fdatasync() for Linux in order to avoid
         * flushing metadata. */
        aof_fsync(appendfd); /* Let's try to get this data on the disk */
        lastfsync = now;
    }

    aofLock();
    aofUnblockClients(blocked_clients);
    listRelease(blocked_clients);

    server.aof.lastfsync = lastfsync;
    /* server.aof is locked on return */
}

sds catAppendOnlyGenericCommand(sds buf, int argc, robj **argv) {
    int j;
    buf = sdscatprintf(buf,"*%d\r\n",argc);
    for (j = 0; j < argc; j++) {
        robj *o = getDecodedObject(argv[j]);
        buf = sdscatprintf(buf,"$%lu\r\n",(unsigned long)sdslen(o->ptr));
        buf = sdscatlen(buf,o->ptr,sdslen(o->ptr));
        buf = sdscatlen(buf,"\r\n",2);
        decrRefCount(o);
    }
    return buf;
}

sds catAppendOnlyExpireAtCommand(sds buf, robj *key, robj *seconds) {
    int argc = 3;
    long when;
    robj *argv[3];

    /* Make sure we can use strtol */
    seconds = getDecodedObject(seconds);
    when = time(NULL)+strtol(seconds->ptr,NULL,10);
    decrRefCount(seconds);

    argv[0] = createStringObject("EXPIREAT",8);
    argv[1] = key;
    argv[2] = createObject(REDIS_STRING,
        sdscatprintf(sdsempty(),"%ld",when));
    buf = catAppendOnlyGenericCommand(buf, argc, argv);
    decrRefCount(argv[0]);
    decrRefCount(argv[2]);
    return buf;
}

void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc) {
    sds buf = sdsempty();
    robj *tmpargv[3];

    /* The DB this command was targetting is not the same as the last command
     * we appendend. To issue a SELECT command is needed. */
    if (dictid != server.appendseldb) {
        char seldb[64];

        snprintf(seldb,sizeof(seldb),"%d",dictid);
        buf = sdscatprintf(buf,"*2\r\n$6\r\nSELECT\r\n$%lu\r\n%s\r\n",
            (unsigned long)strlen(seldb),seldb);
        server.appendseldb = dictid;
    }

    if (cmd->proc == expireCommand) {
        /* Translate EXPIRE into EXPIREAT */
        buf = catAppendOnlyExpireAtCommand(buf,argv[1],argv[2]);
    } else if (cmd->proc == setexCommand) {
        /* Translate SETEX to SET and EXPIREAT */
        tmpargv[0] = createStringObject("SET",3);
        tmpargv[1] = argv[1];
        tmpargv[2] = argv[3];
        buf = catAppendOnlyGenericCommand(buf,3,tmpargv);
        decrRefCount(tmpargv[0]);
        buf = catAppendOnlyExpireAtCommand(buf,argv[1],argv[2]);
    } else {
        buf = catAppendOnlyGenericCommand(buf,argc,argv);
    }

    /* Append to the AOF buffer. This will be flushed on disk by the write
     * thread. aofClientCommand() handles optional client blocking. */
    aofLock();
    server.aof.aofbuf = sdscatlen(server.aof.aofbuf,buf,sdslen(buf));

    /* Disable fsync? */
    if (server.no_appendfsync_on_rewrite &&
        (server.bgrewritechildpid != -1 || server.bgsavechildpid != -1))
    {
        server.aof.no_appendfsync = 1;
    } else {
        server.aof.no_appendfsync = 0;
    }

    /* If the buffer is too large, wait until it isn't anymore */
    size_t maxsize = server.aof_write_buffer_max_size;
    int wasfull = 0;
    while (maxsize && sdslen(server.aof.aofbuf) > maxsize) {
        wasfull = 1;
        redisLog(REDIS_NOTICE, "AOF write buffer is full (%lld bytes).",
                 (long long)sdslen(server.aof.aofbuf));
        pthread_cond_signal(&server.aof.writecond);
        pthread_cond_wait(&server.aof.writecond, &server.aof.mutex);
    }
    if (wasfull) {
        redisLog(REDIS_NOTICE, "AOF write buffer no longer full.");
    }
    aofUnlock();

    /* If a background append only file rewriting is in progress we want to
     * accumulate the differences between the child DB and the current one
     * in a buffer, so that when the child process will do its work we
     * can append the differences to the new append only file. */
    if (server.bgrewritechildpid != -1)
        server.bgrewritebuf = sdscatlen(server.bgrewritebuf,buf,sdslen(buf));

    sdsfree(buf);
}

/* Blocks clients if they should wait for AOF disk I/O to finish */
void aofClientCommand(redisClient *c, long long dirty) {
    aofLock();
    /* Put client on hold until AOF data is written to disk */
    c->flags |= REDIS_IO_WAIT;
    aeDeleteFileEvent(server.el,c->fd,AE_READABLE|AE_WRITABLE);
    listAddNodeTail(server.aof.blocked_clients, c);
    aofUnlock();
}

/* In Redis commands are always executed in the context of a client, so in
 * order to load the append only file we need to create a fake client. */
struct redisClient *createFakeClient(void) {
    struct redisClient *c = zmalloc(sizeof(*c));

    selectDb(c,0);
    c->fd = -1;
    c->querybuf = sdsempty();
    c->argc = 0;
    c->argv = NULL;
    c->bufpos = 0;
    c->flags = 0;
    /* We set the fake client as a slave waiting for the synchronization
     * so that Redis will not try to send replies to this client. */
    c->replstate = REDIS_REPL_WAIT_BGSAVE_START;
    c->reply = listCreate();
    c->watched_keys = listCreate();
    listSetFreeMethod(c->reply,decrRefCount);
    listSetDupMethod(c->reply,dupClientReplyValue);
    initClientMultiState(c);
    return c;
}

void freeFakeClient(struct redisClient *c) {
    sdsfree(c->querybuf);
    listRelease(c->reply);
    listRelease(c->watched_keys);
    freeClientMultiState(c);
    zfree(c);
}

/* Replay the append log file. On error REDIS_OK is returned. On non fatal
 * error (the append only file is zero-length) REDIS_ERR is returned. On
 * fatal error an error message is logged and the program exists. */
int loadAppendOnlyFile(char *filename) {
    struct redisClient *fakeClient;
    FILE *fp = fopen(filename,"r");
    struct redis_stat sb;
    int appendonly = server.appendonly;
    long loops = 0;

    /* Start write thread */
    if (fp && aofStartWriteThread() == REDIS_ERR) {
        redisLog(REDIS_WARNING,"Fatal error: can't create AOF write thread: %s",strerror(errno));
        fclose(fp);
        exit(1);
    }

    if (fp && redis_fstat(fileno(fp),&sb) != -1 && sb.st_size == 0) {
        aofLock();
        server.aof.appendonly_current_size = 0;
        aofUnlock();
        fclose(fp);
        return REDIS_ERR;
    }

    if (fp == NULL) {
        redisLog(REDIS_WARNING,"Fatal error: can't open the append log file for reading: %s",strerror(errno));
        exit(1);
    }

    /* Temporarily disable AOF, to prevent EXEC from feeding a MULTI
     * to the same file we're about to read. */
    server.appendonly = 0;

    fakeClient = createFakeClient();
    startLoading(fp);

    while(1) {
        int argc, j;
        unsigned long len;
        robj **argv;
        char buf[128];
        sds argsds;
        struct redisCommand *cmd;

        /* Serve the clients from time to time */
        if (!(loops++ % 1000)) {
            loadingProgress(ftello(fp));
            aeProcessEvents(server.el, AE_FILE_EVENTS|AE_DONT_WAIT);
        }

        if (fgets(buf,sizeof(buf),fp) == NULL) {
            if (feof(fp))
                break;
            else
                goto readerr;
        }
        if (buf[0] != '*') goto fmterr;
        argc = atoi(buf+1);
        argv = zmalloc(sizeof(robj*)*argc);
        for (j = 0; j < argc; j++) {
            if (fgets(buf,sizeof(buf),fp) == NULL) goto readerr;
            if (buf[0] != '$') goto fmterr;
            len = strtol(buf+1,NULL,10);
            argsds = sdsnewlen(NULL,len);
            if (len && fread(argsds,len,1,fp) == 0) goto fmterr;
            argv[j] = createObject(REDIS_STRING,argsds);
            if (fread(buf,2,1,fp) == 0) goto fmterr; /* discard CRLF */
        }

        /* Command lookup */
        cmd = lookupCommand(argv[0]->ptr);
        if (!cmd) {
            redisLog(REDIS_WARNING,"Unknown command '%s' reading the append only file", argv[0]->ptr);
            exit(1);
        }
        /* Run the command in the context of a fake client */
        fakeClient->argc = argc;
        fakeClient->argv = argv;
        cmd->proc(fakeClient);

        /* The fake client should not have a reply */
        redisAssert(fakeClient->bufpos == 0 && listLength(fakeClient->reply) == 0);

        /* Clean up. Command code may have changed argv/argc so we use the
         * argv/argc of the client instead of the local variables. */
        for (j = 0; j < fakeClient->argc; j++)
            decrRefCount(fakeClient->argv[j]);
        zfree(fakeClient->argv);
    }

    /* This point can only be reached when EOF is reached without errors.
     * If the client is in the middle of a MULTI/EXEC, log error and quit. */
    if (fakeClient->flags & REDIS_MULTI) goto readerr;

    fclose(fp);
    freeFakeClient(fakeClient);
    server.appendonly = appendonly;
    stopLoading();
    aofUpdateCurrentSize();
    aofLock();
    server.aof.auto_aofrewrite_base_size = server.aof.appendonly_current_size;
    aofUnlock();
    return REDIS_OK;

readerr:
    if (feof(fp)) {
        redisLog(REDIS_WARNING,"Unexpected end of file reading the append only file");
    } else {
        redisLog(REDIS_WARNING,"Unrecoverable error reading the append only file: %s", strerror(errno));
    }
    exit(1);
fmterr:
    redisLog(REDIS_WARNING,"Bad file format reading the append only file: make a backup of your AOF file, then use ./redis-check-aof --fix <filename>");
    exit(1);
}

/* Write a sequence of commands able to fully rebuild the dataset into
 * "filename". Used both by REWRITEAOF and BGREWRITEAOF. */
int rewriteAppendOnlyFile(char *filename) {
    dictIterator *di = NULL;
    dictEntry *de;
    FILE *fp;
    char tmpfile[256];
    int j;
    time_t now = time(NULL);

    /* Note that we have to use a different temp name here compared to the
     * one used by rewriteAppendOnlyFileBackground() function. */
    snprintf(tmpfile,256,"temp-rewriteaof-%d.aof", (int) getpid());
    fp = fopen(tmpfile,"w");
    if (!fp) {
        redisLog(REDIS_WARNING, "Failed rewriting the append only file: %s", strerror(errno));
        return REDIS_ERR;
    }
    for (j = 0; j < server.dbnum; j++) {
        char selectcmd[] = "*2\r\n$6\r\nSELECT\r\n";
        redisDb *db = server.db+j;
        dict *d = db->dict;
        if (dictSize(d) == 0) continue;
        di = dictGetIterator(d);
        if (!di) {
            fclose(fp);
            return REDIS_ERR;
        }

        /* SELECT the new DB */
        if (fwrite(selectcmd,sizeof(selectcmd)-1,1,fp) == 0) goto werr;
        if (fwriteBulkLongLong(fp,j) == 0) goto werr;

        /* Iterate this DB writing every entry */
        while((de = dictNext(di)) != NULL) {
            sds keystr;
            robj key, *o;
            time_t expiretime;

            keystr = dictGetEntryKey(de);
            o = dictGetEntryVal(de);
            initStaticStringObject(key,keystr);

            expiretime = getExpire(db,&key);

            /* Save the key and associated value */
            if (o->type == REDIS_STRING) {
                /* Emit a SET command */
                char cmd[]="*3\r\n$3\r\nSET\r\n";
                if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                /* Key and value */
                if (fwriteBulkObject(fp,&key) == 0) goto werr;
                if (fwriteBulkObject(fp,o) == 0) goto werr;
            } else if (o->type == REDIS_LIST) {
                /* Emit the RPUSHes needed to rebuild the list */
                char cmd[]="*3\r\n$5\r\nRPUSH\r\n";
                if (o->encoding == REDIS_ENCODING_ZIPLIST) {
                    unsigned char *zl = o->ptr;
                    unsigned char *p = ziplistIndex(zl,0);
                    unsigned char *vstr;
                    unsigned int vlen;
                    long long vlong;

                    while(ziplistGet(p,&vstr,&vlen,&vlong)) {
                        if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                        if (fwriteBulkObject(fp,&key) == 0) goto werr;
                        if (vstr) {
                            if (fwriteBulkString(fp,(char*)vstr,vlen) == 0)
                                goto werr;
                        } else {
                            if (fwriteBulkLongLong(fp,vlong) == 0)
                                goto werr;
                        }
                        p = ziplistNext(zl,p);
                    }
                } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
                    list *list = o->ptr;
                    listNode *ln;
                    listIter li;

                    listRewind(list,&li);
                    while((ln = listNext(&li))) {
                        robj *eleobj = listNodeValue(ln);

                        if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                        if (fwriteBulkObject(fp,&key) == 0) goto werr;
                        if (fwriteBulkObject(fp,eleobj) == 0) goto werr;
                    }
                } else {
                    redisPanic("Unknown list encoding");
                }
            } else if (o->type == REDIS_SET) {
                char cmd[]="*3\r\n$4\r\nSADD\r\n";

                /* Emit the SADDs needed to rebuild the set */
                if (o->encoding == REDIS_ENCODING_INTSET) {
                    int ii = 0;
                    int64_t llval;
                    while(intsetGet(o->ptr,ii++,&llval)) {
                        if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                        if (fwriteBulkObject(fp,&key) == 0) goto werr;
                        if (fwriteBulkLongLong(fp,llval) == 0) goto werr;
                    }
                } else if (o->encoding == REDIS_ENCODING_HT) {
                    dictIterator *di = dictGetIterator(o->ptr);
                    dictEntry *de;
                    while((de = dictNext(di)) != NULL) {
                        robj *eleobj = dictGetEntryKey(de);
                        if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                        if (fwriteBulkObject(fp,&key) == 0) goto werr;
                        if (fwriteBulkObject(fp,eleobj) == 0) goto werr;
                    }
                    dictReleaseIterator(di);
                } else {
                    redisPanic("Unknown set encoding");
                }
            } else if (o->type == REDIS_ZSET) {
                /* Emit the ZADDs needed to rebuild the sorted set */
                char cmd[]="*4\r\n$4\r\nZADD\r\n";

                if (o->encoding == REDIS_ENCODING_ZIPLIST) {
                    unsigned char *zl = o->ptr;
                    unsigned char *eptr, *sptr;
                    unsigned char *vstr;
                    unsigned int vlen;
                    long long vll;
                    double score;

                    eptr = ziplistIndex(zl,0);
                    redisAssert(eptr != NULL);
                    sptr = ziplistNext(zl,eptr);
                    redisAssert(sptr != NULL);

                    while (eptr != NULL) {
                        redisAssert(ziplistGet(eptr,&vstr,&vlen,&vll));
                        score = zzlGetScore(sptr);

                        if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                        if (fwriteBulkObject(fp,&key) == 0) goto werr;
                        if (fwriteBulkDouble(fp,score) == 0) goto werr;
                        if (vstr != NULL) {
                            if (fwriteBulkString(fp,(char*)vstr,vlen) == 0)
                                goto werr;
                        } else {
                            if (fwriteBulkLongLong(fp,vll) == 0)
                                goto werr;
                        }
                        zzlNext(zl,&eptr,&sptr);
                    }
                } else if (o->encoding == REDIS_ENCODING_SKIPLIST) {
                    zset *zs = o->ptr;
                    dictIterator *di = dictGetIterator(zs->dict);
                    dictEntry *de;

                    while((de = dictNext(di)) != NULL) {
                        robj *eleobj = dictGetEntryKey(de);
                        double *score = dictGetEntryVal(de);

                        if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                        if (fwriteBulkObject(fp,&key) == 0) goto werr;
                        if (fwriteBulkDouble(fp,*score) == 0) goto werr;
                        if (fwriteBulkObject(fp,eleobj) == 0) goto werr;
                    }
                    dictReleaseIterator(di);
                } else {
                    redisPanic("Unknown sorted set encoding");
                }
            } else if (o->type == REDIS_HASH) {
                char cmd[]="*4\r\n$4\r\nHSET\r\n";

                /* Emit the HSETs needed to rebuild the hash */
                if (o->encoding == REDIS_ENCODING_ZIPMAP) {
                    unsigned char *p = zipmapRewind(o->ptr);
                    unsigned char *field, *val;
                    unsigned int flen, vlen;

                    while((p = zipmapNext(p,&field,&flen,&val,&vlen)) != NULL) {
                        if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                        if (fwriteBulkObject(fp,&key) == 0) goto werr;
                        if (fwriteBulkString(fp,(char*)field,flen) == 0)
                            goto werr;
                        if (fwriteBulkString(fp,(char*)val,vlen) == 0)
                            goto werr;
                    }
                } else {
                    dictIterator *di = dictGetIterator(o->ptr);
                    dictEntry *de;

                    while((de = dictNext(di)) != NULL) {
                        robj *field = dictGetEntryKey(de);
                        robj *val = dictGetEntryVal(de);

                        if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                        if (fwriteBulkObject(fp,&key) == 0) goto werr;
                        if (fwriteBulkObject(fp,field) == 0) goto werr;
                        if (fwriteBulkObject(fp,val) == 0) goto werr;
                    }
                    dictReleaseIterator(di);
                }
            } else {
                redisPanic("Unknown object type");
            }
            /* Save the expire time */
            if (expiretime != -1) {
                char cmd[]="*3\r\n$8\r\nEXPIREAT\r\n";
                /* If this key is already expired skip it */
                if (expiretime < now) continue;
                if (fwrite(cmd,sizeof(cmd)-1,1,fp) == 0) goto werr;
                if (fwriteBulkObject(fp,&key) == 0) goto werr;
                if (fwriteBulkLongLong(fp,expiretime) == 0) goto werr;
            }
        }
        dictReleaseIterator(di);
    }

    /* Make sure data will not remain on the OS's output buffers */
    fflush(fp);
    aof_fsync(fileno(fp));
    fclose(fp);

    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. */
    if (rename(tmpfile,filename) == -1) {
        redisLog(REDIS_WARNING,"Error moving temp append only file on the final destination: %s", strerror(errno));
        unlink(tmpfile);
        return REDIS_ERR;
    }
    redisLog(REDIS_NOTICE,"SYNC append only file rewrite performed");
    return REDIS_OK;

werr:
    fclose(fp);
    unlink(tmpfile);
    redisLog(REDIS_WARNING,"Write error writing append only file on disk: %s", strerror(errno));
    if (di) dictReleaseIterator(di);
    return REDIS_ERR;
}

/* This is how rewriting of the append only file in background works:
 *
 * 1) The user calls BGREWRITEAOF
 * 2) Redis calls this function, that forks():
 *    2a) the child rewrite the append only file in a temp file.
 *    2b) the parent accumulates differences in server.bgrewritebuf.
 * 3) When the child finished '2a' exists.
 * 4) The parent will trap the exit code, if it's OK, will append the
 *    data accumulated into server.bgrewritebuf into the temp file, and
 *    finally will rename(2) the temp file in the actual file name.
 *    The the new file is reopened as the new append only file. Profit!
 */
int rewriteAppendOnlyFileBackground(void) {
    pid_t childpid;
    long long start;

    if (server.bgrewritechildpid != -1) return REDIS_ERR;
    if (server.ds_enabled != 0) {
        redisLog(REDIS_WARNING,"BGREWRITEAOF called with diskstore enabled: AOF is not supported when diskstore is enabled. Operation not performed.");
        return REDIS_ERR;
    }
    start = ustime();
    if ((childpid = fork()) == 0) {
        char tmpfile[256];

        /* Child */
        if (server.ipfd > 0) close(server.ipfd);
        if (server.sofd > 0) close(server.sofd);
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) getpid());
        if (rewriteAppendOnlyFile(tmpfile) == REDIS_OK) {
            _exit(0);
        } else {
            _exit(1);
        }
    } else {
        /* Parent */
        server.stat_fork_time = ustime()-start;
        if (childpid == -1) {
            redisLog(REDIS_WARNING,
                "Can't rewrite append only file in background: fork: %s",
                strerror(errno));
            return REDIS_ERR;
        }
        redisLog(REDIS_NOTICE,
            "Background append only file rewriting started by pid %d",childpid);
        server.bgrewritechildpid = childpid;
        updateDictResizePolicy();
        /* We set appendseldb to -1 in order to force the next call to the
         * feedAppendOnlyFile() to issue a SELECT command, so the differences
         * accumulated by the parent into server.bgrewritebuf will start
         * with a SELECT statement and it will be safe to merge. */
        server.appendseldb = -1;
        return REDIS_OK;
    }
    return REDIS_OK; /* unreached */
}

void bgrewriteaofCommand(redisClient *c) {
    if (server.bgrewritechildpid != -1) {
        addReplyError(c,"Background append only file rewriting already in progress");
    } else if (server.bgsavechildpid != -1) {
        server.aofrewrite_scheduled = 1;
        addReplyStatus(c,"Background append only file rewriting scheduled");
    } else if (rewriteAppendOnlyFileBackground() == REDIS_OK) {
        addReplyStatus(c,"Background append only file rewriting started");
    } else {
        addReply(c,shared.err);
    }
}

void aofRemoveTempFile(pid_t childpid) {
    char tmpfile[256];

    snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) childpid);
    unlink(tmpfile);
}

/* Update the server.appendonly_current_size filed explicitly using stat(2)
 * to check the size of the file. This is useful after a rewrite or after
 * a restart, normally the size is updated just adding the write length
 * to the current lenght, that is much faster. */
void aofUpdateCurrentSize(void) {
    struct redis_stat sb;
    aofLock();
    int appendfd = server.aof.appendfd;
    aofUnlock();
    if (redis_fstat(appendfd,&sb) == -1) {
        redisLog(REDIS_WARNING,"Unable to check the AOF length: %s",
            strerror(errno));
    } else {
        aofLock();
        server.aof.appendonly_current_size = sb.st_size;
        aofUnlock();
    }
}

/* Called by the write thread to finish a rewrite. server.aof should be locked
 * before calling this and will also be locked when the function returns. */
void aofWriteThreadHandleRewriteDone(void) {
    int fd;
    char tmpfile[256];
    pid_t bgrewritechildpid = server.aof.bgrewritechildpid;
    sds buf = server.aof.aofbuf;
    server.aof.aofbuf = sdsempty();
    server.aof.writestate = REDIS_AOF_WRITE_THREAD_ACTIVE;
    aofUnlock();

    /* Wake up the main thread, if it is blocking because of a full buffer */
    pthread_cond_signal(&server.aof.writecond);

    /* Now it's time to flush the differences accumulated by the parent */
    snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int)bgrewritechildpid);
    fd = open(tmpfile,O_WRONLY|O_APPEND);
    if (fd == -1) {
        redisLog(REDIS_WARNING, "Not able to open the temp append only file produced by the child: %s", strerror(errno));
        goto cleanup;
    }
    /* Flush our data... */
    if (write(fd,buf,sdslen(buf)) !=
            (signed) sdslen(buf)) {
        redisLog(REDIS_WARNING, "Error or short write trying to flush the parent diff of the append log file in the child temp file: %s", strerror(errno));
        close(fd);
        goto cleanup;
    }
    aof_fsync(fd); /* Make sure the new file has reached the disk */
    redisLog(REDIS_NOTICE,"Parent diff flushed into the new append log file with success (%lu bytes)",sdslen(buf));
    /* Now our work is to rename the temp file into the stable file. And switch
     * the file descriptor used by the server for append only. Note that we use
     * server.appendfilename here without locking. It is shared between
     * threads, but never changes during operation. */
    if (rename(tmpfile,server.appendfilename) == -1) {
        redisLog(REDIS_WARNING,"Can't rename the temp append only file into the stable one: %s", strerror(errno));
        close(fd);
        goto cleanup;
    }
    /* Mission completed... almost */
    redisLog(REDIS_NOTICE,"Append only file successfully rewritten.");
    aofLock();
    server.aof.lastfsync = time(NULL);
    int appendfd = server.aof.appendfd;
    aofUnlock();
    if (appendfd != -1) {
        /* If append only is actually enabled... */
        close(appendfd);
        aofLock();
        server.aof.appendfd = fd;
        aofUnlock();
        redisLog(REDIS_NOTICE,"The new append only file was selected for future appends.");
        aofUpdateCurrentSize();
        aofLock();
        server.aof.auto_aofrewrite_base_size = server.aof.appendonly_current_size;
        aofUnlock();
    } else {
        /* If append only is disabled we just generate a dump in this
         * format. Why not? */
        close(fd);
    }
cleanup:
    sdsfree(buf);
    aofRemoveTempFile(bgrewritechildpid);
    aofLock();
    server.aof.bgrewritechildpid = -1;
    server.aof.bgrewrite_finished = 1;
    /* server.aof is locked on return */
}

/* A background append only file rewriting (BGREWRITEAOF) terminated its work.
 * Handle this. */
void backgroundRewriteDoneHandler(int exitcode, int bysignal) {
    if (!bysignal && exitcode == 0) {
        redisLog(REDIS_NOTICE,
            "Background append only file rewriting terminated with success");
        /* Start write thread, if not started */
        if (aofStartWriteThread()) {
            redisLog(REDIS_WARNING,"Failed to start AOF write thread");
        }
        aofLock();
        /* Make sure the write thread is ready to handle the rewrite */
        if (server.aof.writestate == REDIS_AOF_WRITE_THREAD_ACTIVE) {
            /* Tell the write thread to handle the rewrite */
            server.aof.writestate = REDIS_AOF_WRITE_THREAD_REWRITE;
            server.aof.bgrewritechildpid = server.bgrewritechildpid;
            /* Replace aofbuf with bgrewritebuf */
            sdsfree(server.aof.aofbuf);
            server.aof.aofbuf = server.bgrewritebuf;
            server.bgrewritebuf = sdsempty();
            /* Release lock and continue on */
            pthread_cond_signal(&server.aof.writecond);
            aofUnlock();
            server.appendseldb = -1; /* Make sure we will issue SELECT */
            return;
        } else {
            redisLog(REDIS_WARNING, "Background append only file rewriting aborted because write thread wasn't ready");
        }
        aofUnlock();
    } else if (!bysignal && exitcode != 0) {
        redisLog(REDIS_WARNING, "Background append only file rewriting error");
    } else {
        redisLog(REDIS_WARNING,
            "Background append only file rewriting terminated by signal %d",
            bysignal);
    }
    /* Clean up (only on failure) */
    sdsfree(server.bgrewritebuf);
    server.bgrewritebuf = sdsempty();
    aofRemoveTempFile(server.bgrewritechildpid);
    server.bgrewritechildpid = -1;
}
