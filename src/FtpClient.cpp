// matth-x/ArduinoOcppMongoose
// Copyright Matthias Akstaller 2019 - 2023
// GPL-3.0 License (see LICENSE)

#include "FtpClient.h"
#include <ArduinoOcpp/Debug.h>
#include <ArduinoOcpp/Platform.h>

using namespace ArduinoOcpp;

void ftp_ctrl_cb(struct mg_connection *c, int ev, void *ev_data, void *fn_data);
void ftp_data_cb(struct mg_connection *c, int ev, void *ev_data, void *fn_data);

#if defined(AO_MG_VERSION_614)
void close_mg_conn(mg_connection *c) {
    c->flags |= MG_F_SEND_AND_CLOSE;
}

#define MG_COMPAT_EV_READ MG_EV_RECV
#define MG_COMPAT_RECV recv_mbuf
#define MG_COMPAT_SEND send_mbuf
#define MG_COMPAT_FN_DATA user_data
#else
void close_mg_conn(mg_connection *c) {
    c->is_draining = 1;
}

#define MG_COMPAT_EV_READ MG_EV_READ
#define MG_COMPAT_RECV recv
#define MG_COMPAT_SEND send
#define MG_COMPAT_FN_DATA fn_data
#endif

FtpClient::FtpClient(struct mg_mgr *mgr) : mgr(mgr) {
    AO_DBG_DEBUG("construct");
}

FtpClient::~FtpClient() {
    AO_DBG_DEBUG("destruct");

    if (data_conn) {
        data_conn->MG_COMPAT_FN_DATA = nullptr;
        close_mg_conn(data_conn);
        data_conn = nullptr;
    }

    if (ctrl_conn) {
        ctrl_conn->MG_COMPAT_FN_DATA = nullptr;
        close_mg_conn(ctrl_conn);
        ctrl_conn = nullptr;
    }

    if (onClose) {
        onClose();
        onClose = nullptr;
    }
}

bool FtpClient::getFile(const char *ftp_url_raw, std::function<size_t(const char *data, size_t len)> onReceiveChunk, std::function<void()> onClose) {
    if (!ftp_url_raw) {
        AO_DBG_DEBUG("invalid args");
        return false;
    }

    AO_DBG_DEBUG("init download %s", ftp_url_raw);// ftp://[user[:pass]@]host[:port][/directory]/filename

    std::string ftp_url = ftp_url_raw; //copy input ftp_url

    //tolower protocol specifier
    for (auto c = ftp_url.begin(); *c != ':' && c != ftp_url.end(); c++) {
        *c = tolower(*c);
    }

    //check if protocol supported
    if (!strncmp(ftp_url.c_str(), "ftps", strlen("ftps"))) {
        AO_DBG_ERR("no TLS support. Please use ftp://");
        return false;
    } else if (strncmp(ftp_url.c_str(), "ftp://", strlen("ftp://"))) {
        AO_DBG_ERR("protocol not supported. Please use ftp://");
        return false;
    }

    //parse FTP URL: dir and fname
    auto dir_pos = ftp_url.find_first_of('/', strlen("ftp://"));
    if (dir_pos != std::string::npos) {
        auto fname_pos = ftp_url.find_last_of('/');
        dir = ftp_url.substr(dir_pos, fname_pos - dir_pos);
        fname = ftp_url.substr(fname_pos + 1);
    }
    
    if (fname.empty()) {
        AO_DBG_ERR("missing filename");
        return false;
    }
    
    AO_DBG_DEBUG("parsed dir: %s; fname: %s", dir.c_str(), fname.c_str());

    //parse FTP URL: user, pass, host, port

    std::string user_pass_host_port = ftp_url.substr(strlen("ftp://"), dir_pos - strlen("ftp://"));
    std::string user_pass, host_port;
    auto user_pass_delim = user_pass_host_port.find_first_of('@');
    if (user_pass_delim != std::string::npos) {
        host_port = user_pass_host_port.substr(user_pass_delim + 1);
        user_pass = user_pass_host_port.substr(0, user_pass_delim);
    } else {
        host_port = user_pass_host_port;
    }

    if (!user_pass.empty()) {
        auto user_delim = user_pass.find_first_of(':');
        if (user_delim != std::string::npos) {
            user = user_pass.substr(0, user_delim);
            pass = user_pass.substr(user_delim + 1);
        } else {
            user = user_pass;
        }
    }

    AO_DBG_DEBUG("parsed user: %s; pass: %s", user.c_str(), pass.c_str());

    if (host_port.empty()) {
        AO_DBG_ERR("missing hostname");
        return false;
    }

    if (host_port.find(':') == std::string::npos) {
        //use default port number
        host_port.append(":21");
    }

    url = std::string("tcp://") + host_port;

    AO_DBG_DEBUG("parsed ctrl_ch URL: %s", url.c_str());

    if (ctrl_conn) {
        AO_DBG_DEBUG("close dangling ctrl channel");
        ctrl_conn->MG_COMPAT_FN_DATA = nullptr;
        close_mg_conn(ctrl_conn);
        ctrl_conn = nullptr;
    }

    ctrl_conn = mg_connect(mgr, url.c_str(), ftp_ctrl_cb, this);

    AO_DBG_DEBUG("control conn: %p", ctrl_conn);

    this->onReceiveChunk = onReceiveChunk;
    this->onClose = onClose;

    return ctrl_conn != nullptr;
}

void ftp_ctrl_cb(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
    if (ev != 2) {
        AO_DBG_VERBOSE("Cb fn with event: %d\n", ev);
        (void)0;
    }

#if defined(AO_MG_VERSION_614)
    if (ev == MG_EV_CONNECT && *(int *) ev_data != 0) {
        AO_DBG_WARN("connection error %d", *(int *) ev_data);
        return;
    }
#else
    if (ev == MG_EV_ERROR) {
        MG_ERROR(("%p %s", c->fd, (char *) ev_data));
        AO_DBG_WARN("connection error");
        return;
    }
#endif

    if (!fn_data) {
        if (ev == MG_EV_CLOSE) {
            AO_DBG_INFO("connection closed");
            (void)0;
        } else {
            AO_DBG_ERR("invalid state %d", ev);
            close_mg_conn(c);
            (void)0;
        }
        return;
    }

    FtpClient& session = *reinterpret_cast<FtpClient*>(fn_data);

    if (ev == MG_EV_CONNECT) {
        AO_DBG_WARN("Insecure connection (FTP)");
        AO_DBG_INFO("connection %s -- connected!", session.url.c_str());
        session.ctrl_opened = true;
    } else if (ev == MG_EV_CLOSE) {
        AO_DBG_INFO("connection %s -- closed", session.url.c_str());
        session.ctrl_closed = true;
        if (session.onClose) {
            session.onClose();
            session.onClose = nullptr;
        }
        session.ctrl_conn = nullptr;
        (void)0;
    } else if (ev == MG_COMPAT_EV_READ) {
        // read multi-line command
        char *line_next = (char*) c->MG_COMPAT_RECV.buf;
        while (line_next < (char*) c->MG_COMPAT_RECV.buf + c->MG_COMPAT_RECV.len) {

            // take current line
            char *line = line_next;

            // null-terminate current line and find begin of next line
            while (line_next + 1 < (char*)c->MG_COMPAT_RECV.buf + c->MG_COMPAT_RECV.len && *line_next != '\n') {
                line_next++;
            }
            *line_next = '\0';
            line_next++;

            AO_DBG_DEBUG("<-- %s", line);

            if (!strncmp("530", line, 3) // Not logged in
                    || !strncmp("220", line, 3)) {  // Service ready for new user
                AO_DBG_DEBUG("select user %s", session.user.empty() ? "anonymous" : session.user.c_str());
                mg_printf(c, "USER %s\r\n", session.user.empty() ? "anonymous" : session.user.c_str());
                break;
            } else if (!strncmp("331", line, 3)) { // User name okay, need password
                AO_DBG_DEBUG("enter pass %s", session.pass.c_str());
                mg_printf(c, "PASS %s\r\n", session.pass.c_str());
                break;
            } else if (!strncmp("230", line, 3)) { // User logged in, proceed
                AO_DBG_DEBUG("select directory %s", session.dir.empty() ? "/" : session.dir.c_str());
                mg_printf(c, "CWD %s\r\n", session.dir.empty() ? "/" : session.dir.c_str());
                break;
            } else if (!strncmp("250", line, 3)) { // Requested file action okay, completed
                AO_DBG_DEBUG("enter passive mode");
                mg_printf(c, "PASV\r\n");
                break;
            } else if (!strncmp("227", line, 3)) { // Entering Passive Mode (h1,h2,h3,h4,p1,p2)

                // parse address field. Replace all non-digits by delimiter character ' '
                for (size_t i = 3; line + i < line_next; i++) {
                    if (line[i] < '0' || line[i] > '9') {
                        line[i] = (unsigned char) ' ';
                    }
                }

                unsigned int h1 = 0, h2 = 0, h3 = 0, h4 = 0, p1 = 0, p2 = 0;

                auto ret = sscanf((const char *)c->MG_COMPAT_RECV.buf + 3, "%u %u %u %u %u %u", &h1, &h2, &h3, &h4, &p1, &p2);
                if (ret == 6) {
                    unsigned int port = 256U * p1 + p2;

                    char url [64] = {'\0'};
                    auto ret = snprintf(url, 64, "tcp://%u.%u.%u.%u:%u", h1, h2, h3, h4, port);
                    if (ret < 0 || ret >= 64) {
                        AO_DBG_ERR("url format failure");
                        mg_printf(c, "QUIT\r\n");
                        close_mg_conn(c);
                        break;
                    }
                    AO_DBG_DEBUG("FTP upload address: %s", url);
                    session.data_url = url;

                    if (session.data_conn) {
                        AO_DBG_DEBUG("close dangling data channel");
                        session.data_conn->MG_COMPAT_FN_DATA = nullptr;
                        close_mg_conn(session.data_conn);
                        session.data_conn = nullptr;
                    }

                    session.data_conn = mg_connect(c->mgr, url, ftp_data_cb, &session);

                    if (!session.data_conn) {
                        AO_DBG_ERR("cannot open data ch");
                        mg_printf(c, "QUIT\r\n");
                        close_mg_conn(c);
                        break;
                    }

                    //success -> wait for data_conn to establish connection, ftp_data_cb will send next command
                } else {
                    AO_DBG_ERR("could not process ftp data address");
                    mg_printf(c, "QUIT\r\n");
                    close_mg_conn(c);
                    break;
                }

            } else if (!strncmp("150", line, 3)) { // File status okay; about to open data connection
                AO_DBG_DEBUG("open data connection");
                (void)0;
            } else if (!strncmp("226", line, 3)) { // Closing data connection. Requested file action successful (for example, file transfer or file abort)
                AO_DBG_DEBUG("file action success");
                if (session.data_conn) {
                    close_mg_conn(session.data_conn);
                }
                mg_printf(c, "QUIT\r\n");
                close_mg_conn(c);
                break;
            } else if (!strncmp("55", line, 2)) { // Requested action not taken / aborted
                AO_DBG_DEBUG("file action error");
                if (session.data_conn) {
                    close_mg_conn(session.data_conn);
                }
                mg_printf(c, "QUIT\r\n");
                close_mg_conn(c);
                break;
            } else {
                AO_DBG_ERR("unkown command");
                if (session.data_conn) {
                    close_mg_conn(session.data_conn);
                }
                mg_printf(c, "QUIT\r\n");
                close_mg_conn(c);
                break;
            }

            size_t consumed = line_next - line;

            if (consumed > c->MG_COMPAT_RECV.len) {
                AO_DBG_ERR("invalid state");
                break;
            }

            c->MG_COMPAT_RECV.len -= consumed;
        }
        c->MG_COMPAT_RECV.len = 0;
    }
}

void ftp_data_cb(struct mg_connection *c, int ev, void *ev_data, void *fn_data) {
    if (ev != 2) {
        AO_DBG_VERBOSE("Cb fn with event: %d\n", ev);
        (void)0;
    }

#if defined(AO_MG_VERSION_614)
    if (ev == MG_EV_CONNECT && *(int *) ev_data != 0) {
        AO_DBG_WARN("connection error %d", *(int *) ev_data);
        return;
    }
#else
    if (ev == MG_EV_ERROR) {
        MG_ERROR(("%p %s", c->fd, (char *) ev_data));
        AO_DBG_WARN("connection error");
        return;
    }
#endif

    if (!fn_data) {
        if (ev == MG_EV_CLOSE) {
            AO_DBG_INFO("connection closed");
            (void)0;
        } else {
            AO_DBG_ERR("invalid state %d", ev);
            close_mg_conn(c);
            (void)0;
        }
        return;
    }

    FtpClient& session = *reinterpret_cast<FtpClient*>(fn_data);

    if (ev == MG_EV_CONNECT) {
        AO_DBG_WARN("Insecure connection (FTP)");
        AO_DBG_INFO("connection %s -- connected!", session.data_url.c_str());
        AO_DBG_DEBUG("fetch file %s", session.fname.c_str());
        mg_printf(session.ctrl_conn, "RETR %s\r\n", session.fname.c_str());
    } else if (ev == MG_EV_CLOSE) {
        AO_DBG_INFO("connection %s -- closed", session.data_url.c_str());
        session.data_conn = nullptr;
        (void)0;
    } else if (ev == MG_COMPAT_EV_READ) {
        AO_DBG_DEBUG("read");
        //receive payload
        if (session.onReceiveChunk) {
            auto ret = session.onReceiveChunk((const char *)c->MG_COMPAT_RECV.buf, c->MG_COMPAT_RECV.len);

            if (ret <= c->MG_COMPAT_RECV.len) {
                c->MG_COMPAT_RECV.len -= ret;
            } else {
                AO_DBG_ERR("write error");
                c->MG_COMPAT_RECV.len = 0;
                mg_printf(session.ctrl_conn, "QUIT\r\n");
            }
        } else {
            AO_DBG_ERR("invalid state");
            c->MG_COMPAT_RECV.len = 0;
            mg_printf(session.ctrl_conn, "QUIT\r\n");
        }
    }
}