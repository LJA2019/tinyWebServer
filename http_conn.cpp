#include <sys/uio.h>

#include "http_conn.h"

/* 定义HTTP响应的一些状态信息 */
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

/* 网站的根目录 */
const char *doc_root = "/home/lja/cppproject/tinyweb";

// 设置文件描述符非阻塞
int setnonblocking(int fd) {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int fd, bool one_shot) {
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    if (one_shot) {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

void http_conn::close_conn(bool real_close) {
    if (real_close && m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        --m_user_count; // 关闭一个连接，客户总量减一
    }
}

void http_conn::init(int sockfd, const sockaddr_in &addr) {
    m_sockfd = sockfd;
    m_address = addr;
    /* 如下两行是为了避免TIME_WAIT状态，仅用于调试，实际使用时应该去掉 */
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    addfd(m_epollfd, sockfd, true);
    ++m_user_count;

    init();
}

void http_conn::init() {
    m_check_state = CHECK_STATE::CHECK_STATE_REQUESTLINE;
    m_linger = false;   // 设为 false？
    
    m_method = METHOD::GET;
    m_url = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);

    m_bytes_to_send = 0;
    m_bytes_have_send = 0;
}

/* 从状态机，分析得到一行 */
http_conn::LINE_STATUS http_conn::parse_line() {
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r') {
            if ((m_checked_idx + 1) == m_read_idx) {
                return LINE_STATUS::LINE_OPEN;
            } else if (m_read_buf[m_checked_idx + 1] == '\n') {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_STATUS::LINE_OK;
            }
            return LINE_STATUS::LINE_BAD;
        } else if (temp == '\n') {
            if ((m_checked_idx > 1) && (m_read_buf[m_checked_idx - 1] == '\r')) {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_STATUS::LINE_OK;
            }
            return LINE_STATUS::LINE_BAD;
        }
    }
    return LINE_STATUS::LINE_OPEN;
}

/* ET模式，循环读取客户数据，直到无数据可读或对方关闭连接 */
bool http_conn::read() {
    if (m_read_idx >= READ_BUFFER_SIZE) {
        return false;
    }

    int bytes_read = 0;
    while (true) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1) {
            if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {  // 非阻塞读，未读到数据
                break;
            }
            return false;   // 其他情况则返回
        } else if (bytes_read == 0) {   // 客户端关闭连接
            return false;
        }
        m_read_idx += bytes_read;
    }
    //printf("%s\n", m_read_buf);
    return true;
}

/*
C 库函数 char *strpbrk(const char *str1, const char *str2); 
检索字符串 str1 中第一个匹配字符串 str2 中字符的字符，不包含空结束字符。
也就是说，依次检验字符串 str1 中的字符，当被检验字符在字符串 str2 中也包含时，
则停止检验，并返回该字符位置(指针),如果未找到字符则返回 NULL。

C 库函数 int strcasecmp (const char *s1, const char *s2);，判断字符串是否相等(忽略大小写)

C 库函数 size_t strspn(const char *str1, const char *str2); 检索字符串 str1 
中第一个不在字符串 str2 中出现的字符下标,下标从0开始

C 库函数 int strncasecmp(const char *s1, const char *s2, size_t n);
比较字符串的前n个字符,比较时会自动忽略大小写的差异。
相同则返回0。s1 若大于s2 则返回大于0 的值，s1 若小于s2 则返回小于0 的值。

C 库函数 char *strchr(const char *str, int c); 
在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置（指针）。
如果未找到该字符则返回 NULL。
*/

/* 解析HTTP请求行，获得请求方法、目标URL，以及HTTP版本号 */
/* GET http://www.baidu.com/index.html HTTP/1.0 */
http_conn::HTTP_CODE http_conn::parse_request_line(char *text) {
    m_url = strpbrk(text, " \t");
    if (!m_url) {
        return HTTP_CODE::BAD_REQUEST;
    }
    *m_url++ = '\0';    // m_url 表示字符串 "http://www.baidu.com/index.html HTTP/1.0"

    char *method = text;    // method 表示字符串 "GET"
    if (strcasecmp(method, "GET") == 0) {   // 仅支持GET方法
        m_method == METHOD::GET;
    } else {
        return HTTP_CODE::BAD_REQUEST;
    }

    //m_url += strspn(m_url, " \t");  // m_url 表示字符串 " HTTP/1.0"
    m_version = strpbrk(m_url, " \t");  // version 表示字符串 " HTTP/1.0"
    if (!m_version) {
        return HTTP_CODE::BAD_REQUEST;
    }
    *m_version++ = '\0';
    if (strcasecmp(m_version, "HTTP/1.1") != 0) { // 仅支持 HTTP/1.1
        //printf("not HTTP/1.1\n");
        return HTTP_CODE::BAD_REQUEST;      
    }

    /* 检查URL是否合法 */
    if (strncasecmp(m_url, "http://", 7) == 0) {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    if (!m_url || m_url[0] != '/') {
        //printf("url illegal\n");
        return HTTP_CODE::BAD_REQUEST;
    }
    //printf("The request URL is: %s\n", m_url);   //m_url 表示字符串 "/index.html"  
    
    /* HTTP请求行处理完毕，状态转移到头部字段的分析 */
    m_check_state = CHECK_STATE::CHECK_STATE_HEADER;
    return HTTP_CODE::NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_headers(char *text) {
    /* 遇到空行，表示头部字段解析完毕 */
    if (text[0] == '\0') {
        /* 如果HTTP请求体有消息体，则还需要读取m_content_length字节的消息体，
            状态机转移到CHECK_STATE_CONTENT状态 */
        if (m_content_length != 0) {
            m_check_state = CHECK_STATE::CHECK_STATE_CONTENT;
            return HTTP_CODE::NO_REQUEST;
        }
        return HTTP_CODE::GET_REQUEST;  // 否则，说明已经得到了一个完整的HTTP请求
    /* 处理Connection头部字段 */
    } else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11; // 注意，这里会在 ':'字符 之后的位置，所以下一行加上偏移就行
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    /* 处理Content-Length头部字段 */
    } else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atoi(text);
    /* 处理Host头部字段 */
    } else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    } else {
        printf("oop! unknow header %s\n", text);
    }
    return HTTP_CODE::NO_REQUEST;
}

/* 没有实现正真解析HTTP请求的消息体，只是判断它是否被完整地读入了 */
// 请求体由Content_length判断结束，没有\r\n
http_conn::HTTP_CODE http_conn::parse_content(char *text) {
    if (m_read_idx >= (m_content_length + m_checked_idx)) {
        text[m_content_length] = '\0';
        return HTTP_CODE::GET_REQUEST;
    }
    return HTTP_CODE::NO_REQUEST;
}

/* 主状态机 */
http_conn::HTTP_CODE http_conn::process_read() {
    LINE_STATUS line_status = LINE_STATUS::LINE_OK;
    HTTP_CODE ret = HTTP_CODE::NO_REQUEST;
    char *text = 0;
    // 当在CHECK_STATE_CONTENT状态且请求体行没有读完
    // 或者其他行为 LINE_OK时循环
    while (((m_check_state == CHECK_STATE::CHECK_STATE_CONTENT) && (line_status == LINE_STATUS::LINE_OK)) 
        || ((line_status = parse_line()) == LINE_STATUS::LINE_OK)) {
        text = get_line();
        m_start_line = m_checked_idx;
        //printf("got 1 http line: %s\n", text);

        switch (m_check_state) {
            case CHECK_STATE::CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                if (ret == HTTP_CODE::BAD_REQUEST) {
                    //printf("parse_request_line bad\n");
                    return HTTP_CODE::BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE::CHECK_STATE_HEADER:
            {
                ret = parse_headers(text);
                if (ret == HTTP_CODE::BAD_REQUEST) {
                    //printf("parse_headers bad\n");
                    return HTTP_CODE::BAD_REQUEST;
                } else if (ret == HTTP_CODE::GET_REQUEST) {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE::CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                if (ret == HTTP_CODE::GET_REQUEST) {
                    return do_request();
                }
                line_status = LINE_STATUS::LINE_OPEN;
                break;
            }
            default:
            {
                return HTTP_CODE::INTERNAL_ERROR;
            }
        }
    }
    return HTTP_CODE::NO_REQUEST;
}

/* 当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性。
如果目标文件存在，对所有用户可读，且不是目录，则使用mmap将其映射
到内存地址m_file_address处，并告诉调用者获取文件成功
 */
http_conn::HTTP_CODE http_conn::do_request() {
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    if (stat(m_real_file, &m_file_stat) < 0) {
        return HTTP_CODE::NO_RESOURCE;
    }
    if (!(m_file_stat.st_mode & S_IROTH)) {
        return HTTP_CODE::FORBIDDEN_REQUEST;
    }
    if (S_ISDIR(m_file_stat.st_mode)) {
        return HTTP_CODE::BAD_REQUEST;
    }
    
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return HTTP_CODE::FILE_REQUEST;
}

/* 对内存映射区执行munmap操作 */
void http_conn::unmap() {
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

/* LT和ET触发的时机 */

/*
    struct iovec {
        void *iov_base; // 内存起始地址
        size_t iov_len; // 这块内存的长度
    };
 */

/* 写HTTP响应 */
bool http_conn::write() {
    int temp = 0;
    if (m_bytes_to_send == 0) {
        //printf("send %d bytes\n", m_bytes_have_send);
        modfd(m_epollfd, m_sockfd, EPOLLIN);    // 修改为监听读事件，即重置，缓冲区有就会读
        init();
        return true;
    }
    while (1) {
        //printf("%s\n", m_write_buf);
        temp = writev(m_sockfd, m_iv, m_iv_count); // 先不监听写，写满再监听
        if (temp <= -1) {
            if (errno == EAGAIN) {
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        m_bytes_to_send -= temp;    // 从写缓冲区读了 temp字节
        m_bytes_have_send += temp;  // 已经发送的字节增加

        /* 已发送的字节数大于等于报头 */
        if (m_bytes_have_send >= m_iv[0].iov_len) {
            m_iv[0].iov_len = 0;    // 报头发送完毕，长度清零
            /* m_write_idx为报头长度，
            所以bytes_have_send（已发送的数据量） - m_write_idx（已发送完的报头中的数据量）
            就等于剩余发送文件的大小, m_iv[1]起始位置m_iv[1].iov_base需要增加这个大小的字节
            */
           m_iv[1].iov_base = m_file_address + m_bytes_have_send - m_write_idx;
           m_iv[1].iov_len = m_bytes_to_send;
        /* 报头还没发完，继续发送报头，修改m_iv指向写缓冲区的位置以及待发送的长度以便下次接着发 */
        } else {
            m_iv[0].iov_base = m_write_buf + m_bytes_have_send;
            m_iv[0].iov_len = m_write_idx - m_bytes_have_send;
        }

        /* 发送HTTP响应完毕，根据HTTP请求中的Connection字段决定是否立即关闭连接 */
        if (m_bytes_to_send <= 0) {
            //printf("send %d bytes\n", m_bytes_have_send);
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);
            if (m_linger) {
                init();
                return true;
            } else {
                return false;
            }
        }
    }
}

/*  C语言  可变参数  <stdarg.h>
定义一个函数，最后一个参数为省略号，省略号前面可以设置自定义参数。
在函数定义中创建一个 va_list 类型变量，该类型是在 stdarg.h 头文件中定义的。
使用 int 参数和 va_start 宏来初始化 va_list 变量为一个参数列表。宏 va_start 是在 stdarg.h 头文件中定义的。
使用 va_arg 宏和 va_list 变量来访问参数列表中的每个项。
使用宏 va_end 来清理赋予 va_list 变量的内存。
double average(int num,...) {
    va_list valist;
    double sum = 0.0;
    int i;
    va_start(valist, num);  // 为 num 个参数初始化 valist
    for (i = 0; i < num; i++) { // 访问所有赋给 valist 的参数
       sum += va_arg(valist, int);
    }
    va_end(valist); // 清理为 valist 保留的内存
    return sum/num;
}


int vsnprintf (char * s, size_t n, const char * format, va_list arg );
失败返回复值，如果n太小，则复制n个字符，不包含终止符，返回原串大小（大于等于n），
如果n足够大，返回字符串长度，字符串长度小于等于n-1，因为有终止符
*/

bool http_conn::add_response(const char *format, ...) {
    if (m_write_idx >= WRITE_BUFFER_SIZE) {
        return false;
    }
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    // if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx)) {
    //     return false;
    // }        修改
    if ((len < 0) || (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))) {
        return false;
    }
    m_write_idx += len;
    va_end(arg_list);
    return true;
}

/* 添加状态行 */
bool http_conn::add_status_line(int status, const char *title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

/* 添加头部字段 */
bool http_conn::add_headers(int content_len) {
    // add_content_length(content_len);
    // add_linger();
    // add_blank_line();
    return add_content_length(content_len) && add_linger() && add_blank_line();
}

bool http_conn::add_content_length(int content_len) {
    return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_linger() {
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line() {
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char *content) {
    return add_response("%s", content);
}

/* 根据服务器处理HTTP请求的结果，决定返回给客户端的内容 */
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret) {
        case HTTP_CODE::INTERNAL_ERROR:
        {
            if (!add_status_line(500, error_500_title) || !add_content(error_500_form) || 
            !add_headers(strlen(error_500_form))) {
                return false;
            }
            break;
        }
        case HTTP_CODE::BAD_REQUEST:
        {
            if (!add_status_line(400, error_400_title) || !add_content(error_400_form) || 
            !add_headers(strlen(error_400_form))) {
                return false;
            }
            break;  
        }
        case HTTP_CODE::NO_RESOURCE:
        {
            if (!add_status_line(404, error_404_title) || !add_content(error_404_form) || 
            !add_headers(strlen(error_404_form))) {
                return false;
            }
            break;              
        }
        case HTTP_CODE::FORBIDDEN_REQUEST:
        {
            if (!add_status_line(403, error_404_title) || !add_content(error_403_form) || 
            !add_headers(strlen(error_403_form))) {
                return false;
            }
            break;                 
        }
        case HTTP_CODE::FILE_REQUEST:
        {
            if (!add_status_line(200, ok_200_title)) {
                return false;
            }
            if (m_file_stat.st_size != 0) {
                if (!add_headers(m_file_stat.st_size)) {
                    return false;
                }
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                m_bytes_to_send = m_write_idx + m_file_stat.st_size;    // 设置要发送的字节数
                return true;
            } else {
                const char *ok_string = "<html><body></body></html>";
                if (!add_headers(strlen(ok_string) || !add_content(ok_string))) {
                    return false;
                }
            }
        }
        default:
        {
            return false;
        }
    }
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    m_bytes_to_send = m_write_idx;  // 设置要发送的字节数
    return true;
}

/* 由线程池中的工作线程调用，这是处理HTTP请求的入口函数 */
void http_conn::process() {
    HTTP_CODE read_ret = process_read();
    if (read_ret == HTTP_CODE::NO_REQUEST) {
        //printf("NO REQUEST\n");
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        //printf("close\n");
        close_conn();
    }
    //printf("continue\n");
    modfd(m_epollfd, m_sockfd, EPOLLOUT);
}