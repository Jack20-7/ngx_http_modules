#include "ngx_http.h"
#include "ngx_config.h"
#include "ngx_core.h"


static char* ngx_http_location_count_set(ngx_conf_t* cf,ngx_command_t* cmd,void* conf);
static void* ngx_http_create_location_count_conf(ngx_conf_t* cf);
static ngx_int_t ngx_http_location_count_handler(ngx_http_request_t* r);
static ngx_int_t ngx_http_location_count_shm_init(ngx_shm_zone_t* zone,void* data);
static void ngx_http_location_count_rbtree_insert_value(ngx_rbtree_node_t* temp,ngx_rbtree_node_t* node,ngx_rbtree_node_t* sentinel);

typedef struct{
    ngx_rbtree_t rbtree;                 //红黑树
    ngx_rbtree_node_t sentinel;          //红黑树的哨兵节点
}ngx_http_location_count_shm_t;

typedef struct {
    ssize_t shmsize;                     //需要分配的共享内存的大小
    ngx_slab_pool_t* shpool;             //红黑树的节点就可以直接从这里面获取
    ngx_http_location_count_shm_t* sh;
}ngx_http_location_count_conf_t;

//指定配置文件中的命令
static ngx_command_t location_commands[] = {
    {
       ngx_string("count"),
       NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS, //命令在location下并且没有参数
       ngx_http_location_count_set,  //当发现配置文件中location字段下有count的时候，会调用的回调函数
       NGX_HTTP_LOC_CONF_OFFSET,
       0,
       NULL
    },
    ngx_null_command //结束标识符
};

//在初始化的时候，会调用到的回调函数
//添加模块其实就是编写这些回调函数
static ngx_http_module_t location_ctx = {
    NULL,                    
    //ngx_http_location_init,
    NULL,

    //main
    NULL,
    NULL,

    //server
    NULL,
    NULL,

    //location
    ngx_http_create_location_count_conf,  //加载配置文件的时候，当看到有location字段时，会调用该函数
    NULL,
};

//生成对应的模块
ngx_module_t ngx_http_location_count_module = {
    NGX_MODULE_V1,
    &location_ctx,
    location_commands,
    NGX_HTTP_MODULE,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NGX_MODULE_V1_PADDING
};


//向红黑树中插入节点
static void ngx_http_location_count_rbtree_insert_value(ngx_rbtree_node_t* temp,
                                            ngx_rbtree_node_t* node,ngx_rbtree_node_t* sentinel){
    ngx_rbtree_node_t** p;
    for(;;){
        if(node->key < temp->key){
            p = &temp->left;
        }else if(node->key > temp->key){
            p = &temp->right;
        }else{
            //遇到相同节点就直接结束
            return;
        }
        if(*p == sentinel){
            //找到了合适的位置
            break;
        }
        temp = *p;
    }

    *p = node;
    node->left = sentinel;
    node->right = sentinel;
    node->parent = temp;

    //调整位置
    ngx_rbt_red(node);
}

//根据client ip到红黑树中查找对应的节点，如果存在就value ++，如果不存在就插入对应的节点
static ngx_int_t ngx_http_location_count_lookup(ngx_http_request_t* r,ngx_http_location_count_conf_t* conf,ngx_uint_t key){
    ngx_rbtree_node_t* node,*sentinel;

    node = conf->sh->rbtree.root;
    sentinel = conf->sh->rbtree.sentinel;

    ngx_log_error(NGX_LOG_EMERG,r->connection->log,ngx_errno,"ngx_http_location_count_lookup\n");
    while(node != sentinel){
        if(key < node->key){
            node = node->left;
            continue;
        }else if(key > node->key){
            node = node->right;
            continue;
        }else{
            node->data++;
            return NGX_OK;
        }
    }
    //如果没有找到对应的节点的话
    node = ngx_slab_alloc_locked(conf->shpool,sizeof(ngx_rbtree_node_t));
    if(NULL == node){
        return NGX_ERROR;
    }
    node->key = key;
    node->data = 1;

    ngx_rbtree_insert(&conf->sh->rbtree,node);
    ngx_log_error(NGX_LOG_EMERG,r->connection->log,ngx_errno,"insert success\n");
    return NGX_OK;
}

static int ngx_encode_http_location_count_rb(ngx_http_location_count_conf_t* conf,char* html){
    ngx_rbtree_node_t* node = ngx_rbtree_min(conf->sh->rbtree.root,conf->sh->rbtree.sentinel); //从最小的节点开始
    do{
        char str[INET_ADDRSTRLEN] = {0};
        char buf[128] = {0};

        sprintf(buf,"req from :%s,count:%d <br/>",inet_ntop(AF_INET,&node->key,str,sizeof(str)),node->data);
        strcat(html,buf);
        node = ngx_rbtree_next(&conf->sh->rbtree,node);
    }while(node);
    return NGX_OK;
}

static ngx_int_t ngx_http_location_count_handler(ngx_http_request_t* r){
    u_char html[1024] = {0};
    int len = sizeof(html);

    ngx_rbtree_key_t key = 0;
    struct sockaddr_in* client_addr = (struct sockaddr_in*)r->connection->sockaddr;

    ngx_http_location_count_conf_t* conf = ngx_http_get_module_loc_conf(r,ngx_http_location_count_module);
    key = (ngx_rbtree_key_t)client_addr->sin_addr.s_addr;

    ngx_shmtx_lock(&conf->shpool->mutex);
    ngx_http_location_count_lookup(r,conf,key);
    ngx_shmtx_unlock(&conf->shpool->mutex);

    ngx_encode_http_location_count_rb(conf,(char*)html);  //遍历红黑树，生成返回的response 的body

    r->headers_out.status = 200;
    ngx_str_set(&r->headers_out.content_type,"text/html");
    ngx_http_send_header(r);                              //先返回header

    //body
    ngx_buf_t* b  = ngx_palloc(r->pool,sizeof(ngx_buf_t));
    ngx_chain_t out;
    out.buf = b;
    out.next = NULL;

    b->pos = html;
    b->last = html + len;
    b->memory = 1;
    b->last_buf = 1;

    return ngx_http_output_filter(r,&out);
}

//共享内存初始化完毕之后会调用到的函数
ngx_int_t ngx_http_location_count_shm_init(ngx_shm_zone_t* zone,void* data){
    ngx_http_location_count_conf_t* conf;
    ngx_http_location_count_conf_t* oconf = data;

    conf = (ngx_http_location_count_conf_t*)zone->data;
    if(oconf){
        conf->sh = oconf->sh;
        conf->shpool = oconf->shpool;
        return NGX_OK;
    }

    printf("ngx_http_location_count_shm_init\n");

    conf->shpool = (ngx_slab_pool_t*)zone->shm.addr;
    conf->sh = ngx_slab_alloc(conf->shpool,sizeof(ngx_http_location_count_shm_t));
    if(NULL == conf->sh){
        return NGX_ERROR;
    }

    conf->shpool->data = conf->sh;
    
    //初始化红黑树
    //红黑树的key -- client ip,value->访问次数
    ngx_rbtree_init(&conf->sh->rbtree,&conf->sh->sentinel,
                         ngx_http_location_count_rbtree_insert_value);

    return NGX_OK;
}

//master process在读取配置文件阶段，当前location下面找到count字段的时候，就会调用该函数
static char* ngx_http_location_count_set(ngx_conf_t* cf,ngx_command_t* cmd,void* conf){

    ngx_log_error(NGX_LOG_EMERG,cf->log,ngx_errno,"ngx_http_location_count_set\n");
    ngx_shm_zone_t* shm_zone;
    ngx_str_t name = ngx_string("location_count_slab_shm");

    ngx_http_location_count_conf_t* mconf = (ngx_http_location_count_conf_t*)conf;
    ngx_http_core_loc_conf_t* corecf = NULL;

    mconf->shmsize = 1024 * 1024;     //需要分配的共享内存的大小
    shm_zone = ngx_shared_memory_add(cf,&name,mconf->shmsize,&ngx_http_location_count_module);
    if(NULL == shm_zone){
        return NGX_CONF_ERROR;
    }

    shm_zone->init = ngx_http_location_count_shm_init;     //分配的共享内存在被初始化完毕之后，会调用的回调函数
    shm_zone->data = mconf;

    corecf = ngx_http_conf_get_module_loc_conf(cf,ngx_http_core_module);
    corecf->handler = ngx_http_location_count_handler;     //绑定用户在访问对应的location下的count时会调用的回调函数

    return NGX_CONF_OK;
}

void* ngx_http_create_location_count_conf(ngx_conf_t* cf){
    ngx_http_location_count_conf_t* conf;
    conf = ngx_palloc(cf->pool,sizeof(ngx_http_location_count_conf_t));
    if(NULL == conf){
        return NULL;
    }
    conf->shmsize = 0;
    ngx_log_error(NGX_LOG_EMERG,cf->log,ngx_errno,"ngx_http_create_location_count_conf");

    return conf;
}




