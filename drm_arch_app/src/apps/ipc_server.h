#pragma once
#include <stddef.h>
#include <stdint.h>

/**
 * 在当前线程运行服务循环；通常放进 pthread 线程入口里调用。
 * 返回值：0 正常退出（比如 stop_fd 触发），<0 出错退出
 *
 * stop_fd: 传入 eventfd/pipe 的读端用于停止（可选）。不需要停止机制就传 -1。
 */
void* apps_ipc_server_thread(void* arg);