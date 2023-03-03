# ngx_http_modules
自己实现的nginx模块

### ngx_http_location_count_module
该模块是用来对指定location的访问次数进行计数的
使用方式:
```
location /test{
    count;
}
```

### ngx_http_filter_module
该模块属于是filter模块，用来对向HTTP clint返回的response进行一些自定义操作,这里是在response 的body中添加一些信息
使用方式
```
location /test{
    add_prefix on;
}
```
