
#include "ngx_config.h"
#include "ngx_core.h"
#include "ngx_http.h"

typedef struct{
    ngx_flag_t enable;
}ngx_http_filter_conf_t;

typedef struct{
    ngx_int_t add_prefix;
}ngx_http_filter_ctx_t;

static ngx_int_t ngx_http_filter_init(ngx_conf_t* cf);
static ngx_int_t ngx_http_filter_header_filter(ngx_http_request_t* r);
static ngx_int_t ngx_http_filter_body_filter(ngx_http_request_t* r,ngx_chain_t* in);

//需要在response中添加的前缀
static ngx_str_t filter_prefix = ngx_string("[my prefix filter]");


static void* ngx_http_filter_create_conf(ngx_conf_t* cf){
    ngx_http_filter_conf_t* mcf = NULL;
    mcf = (ngx_http_filter_conf_t*)ngx_pcalloc(cf->pool,sizeof(ngx_http_filter_conf_t));
    if(NULL == mcf){
        return NULL;
    }

    mcf->enable = NGX_CONF_UNSET;
    return mcf;
}

//合并多个location 的conf
static char* ngx_http_filter_merge_conf(ngx_conf_t* cf,void* parent,void* child){
    ngx_http_filter_conf_t* prev = (ngx_http_filter_conf_t*)parent;
    ngx_http_filter_conf_t* conf = (ngx_http_filter_conf_t*)child;

    ngx_conf_merge_value(conf->enable,prev->enable,0);
    return NGX_CONF_OK;
}

ngx_http_module_t ngx_http_filter_module_ctx = {
     NULL,  //pre configuation
     ngx_http_filter_init,  //post configuration

     //master
     NULL, 
     NULL,

     //server
     NULL,
     NULL,

     //location
     ngx_http_filter_create_conf,
     ngx_http_filter_merge_conf
};

static ngx_command_t  ngx_http_filter_commands[] = {
    {
        ngx_string("add_prefix"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LMT_CONF | NGX_CONF_FLAG,
        ngx_conf_set_flag_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_filter_conf_t,enable),
        NULL
    },
    ngx_null_command
};

ngx_module_t ngx_http_filter_module = {
    NGX_MODULE_V1,
    &ngx_http_filter_module_ctx,
    ngx_http_filter_commands,
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

static ngx_http_output_header_filter_pt ngx_http_next_header_filter;
static ngx_http_output_body_filter_pt ngx_http_next_body_filter;

//配置文件解析完毕之后会调用到的函数
//该函数里面主要就是将本模块实现的回调函数添加到 模块链表上面去，以头插法的方式进行添加
static ngx_int_t ngx_http_filter_init(ngx_conf_t* cf){
    ngx_http_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_filter_header_filter;

    ngx_http_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_filter_body_filter;

    ngx_log_error(NGX_LOG_EMERG,cf->log,ngx_errno,"ngx_http_filter_init\n");

    return NGX_OK;
}

//nginx返回HTTP Response的之前，对它Http header会调用的函数
static ngx_int_t ngx_http_filter_header_filter(ngx_http_request_t* r){
    ngx_log_error(NGX_LOG_EMERG,r->connection->log,ngx_errno,"ngx_http_filter_header_filter\n");
    ngx_http_filter_ctx_t* ctx;
    ngx_http_filter_conf_t* conf;

    if(r->headers_out.status != NGX_HTTP_OK){
        return ngx_http_next_header_filter(r);
    }

    ctx = ngx_http_get_module_ctx(r,ngx_http_filter_module);
    if(ctx){
        return ngx_http_next_header_filter(r);
    }

    conf = ngx_http_get_module_loc_conf(r,ngx_http_filter_module);
    if(conf->enable == 0){
        return ngx_http_next_header_filter(r);
    }

    ctx = ngx_pcalloc(r->pool,sizeof(ngx_http_filter_ctx_t));
    if(ctx == NULL){
        return NGX_ERROR;
    }
    ctx->add_prefix = 0;

    ngx_http_set_ctx(r,ctx,ngx_http_filter_module);
    if(r->headers_out.content_type.len >= sizeof("text/html") - 1
            && ngx_strncasecmp(r->headers_out.content_type.data,(u_char*)"text/html",sizeof("text/html") - 1) == 0){

           ngx_log_error(NGX_LOG_EMERG,r->connection->log,ngx_errno,"add_prefix = 1\n");
           ctx->add_prefix = 1;
           if(r->headers_out.content_length_n > 0){
            r->headers_out.content_length_n += filter_prefix.len;
           }
    }
    ngx_log_error(NGX_LOG_EMERG,r->connection->log,ngx_errno,"add_prefix = %d\n",ctx->add_prefix);

    return ngx_http_filter_header_filter(r);
}

static ngx_int_t ngx_http_filter_body_filter(ngx_http_request_t* r,ngx_chain_t* in){
    ngx_http_filter_ctx_t* ctx;
    ctx = ngx_http_get_module_ctx(r,ngx_http_filter_module);
    if(ctx == NULL || ctx->add_prefix != 1){
        return ngx_http_next_body_filter(r,in);
    }

    ctx->add_prefix = 2;
    ngx_buf_t* b = ngx_create_temp_buf(r->pool,filter_prefix.len);
    b->start = b->pos = filter_prefix.data;
    b->last = b->pos + filter_prefix.len;

    ngx_chain_t* c1 = ngx_alloc_chain_link(r->pool);
    c1->buf = b;
    c1->next = in;

    return ngx_http_next_body_filter(r,c1);
}