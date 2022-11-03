#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string>
#include <cstring>
#include <iostream>
#include <fstream>
#include <cassert>
#include <vector>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <sstream>

using namespace std;

#define DEBUG
#define HPPTPORT 80
#define HPPTSPORT 443
#define BUFSZ 4096
#define METHODSZ 128
#define FILENAMESZ 256
#define LINELEN 1024
#define SHORTSZ 128
#define FILESZ (1 << 14)

#ifdef DEBUG
  #define LOG(...) std::cout << __VA_ARGS__
#else
  #define LOG(...)
#endif

typedef struct HTTPMsg{
  int status;
  string status_msg;
  int length;
  vector<string> header;
  string body;
}HttpMsg;

string makeHttpResponse(HttpMsg* msg) {
  string response = "HTTP/1.1";
  response += " " + to_string(msg->status) + " " + msg->status_msg + "\r\n";
  response += "content-length: " + to_string(msg->body.length()) + "\r\n";
  for(int i = 0; i < msg->header.size(); i++) {
    response += msg->header[i] + "\r\n";
  }
  response += "\r\n" + msg->body;
  return response;
}

void requestHttp(int sockfd) {
  char buf[BUFSZ];
  memset(buf, 0, sizeof(buf));
  read(sockfd, buf, BUFSZ);
  LOG( "[\n" + string(buf)  + "]\n" );

  string buf_str = buf;
  assert(buf_str.find("HTTP/") != string::npos);
  int pos_start = 0, pos_end = 0;
  assert((pos_end = buf_str.find(" /")) != string::npos);
  string method = buf_str.substr(pos_start, pos_end-pos_start);

  pos_start = pos_end += 2;
  assert((pos_end = buf_str.find(" ", pos_start)) != string::npos);
  string filename = buf_str.substr(pos_start, pos_end-pos_start);

  assert((pos_start = buf_str.find("Host: ")) != string::npos);
  pos_start += 6;
  assert((pos_end = buf_str.find("\r", pos_start)) != string::npos);
  string host = buf_str.substr(pos_start, pos_end-pos_start);

  if(method == "GET") {
    HttpMsg msg = {.status = 301, .status_msg = "Moved Permanently"};
    msg.header.push_back("Location: https://" + host + "/" + filename);
    string http_resp = makeHttpResponse(&msg);
    LOG("(" + http_resp + ")\n");
    send(sockfd, http_resp.c_str(), http_resp.length(), 0);

  }
}

void* http_server(void* args) {
  int httpfd = socket(PF_INET, SOCK_STREAM, 0);
  assert(httpfd != -1);

  sockaddr_in http_addr = {.sin_family = AF_INET, .sin_port = (in_port_t)htons(HPPTPORT), .sin_addr = {.s_addr=htonl(INADDR_ANY)}};
  assert(bind(httpfd, (struct sockaddr*)&http_addr, sizeof(http_addr)) != -1);
  assert(listen(httpfd, 10) != -1);

  while(1) {
    sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int clientfd = accept(httpfd, (struct sockaddr*) &client_addr, &client_len);
    LOG("port=" + to_string(client_addr.sin_port) + " addr=" + to_string(client_addr.sin_addr.s_addr) + " fd=", to_string(clientfd) + "\n");
    assert(clientfd != -1);
    requestHttp(clientfd);
  }
  return 0;
}

void parseRange(string msg, int* start, int* end) {
  int pos_start = msg.find("Range:");
  if(pos_start == string::npos) return;
  pos_start += 13;   // Range: bytes=
  int pos_end = msg.find("\r", pos_start);
  int pos_mid = msg.find("-", pos_start);
  *start = pos_start == pos_mid ? 0 : stoi(msg.substr(pos_start, pos_mid - pos_start));
  *end = pos_mid == (pos_end - 1) ? INT32_MAX : stoi(msg.substr(pos_mid + 1, pos_end - pos_mid - 1));
}

void requestHttps(SSL* ssl) {
  char buf[BUFSZ];
  memset(buf, 0, sizeof(buf));
  int read_count = SSL_read(ssl, buf, BUFSZ);
  
  LOG( "[\n" + string(buf) + "]\n");
  string buf_str = buf;
  assert(buf_str.find("HTTP/") != string::npos);
  int pos_start = 0, pos_end = 0;
  assert((pos_end = buf_str.find(" /")) != string::npos);
  string method = buf_str.substr(pos_start, pos_end-pos_start);

  pos_start = pos_end += 2;
  assert((pos_end = buf_str.find(" ", pos_start)) != string::npos);
  string filename = buf_str.substr(pos_start, pos_end-pos_start);
  assert((pos_start = buf_str.find("Host: ")) != string::npos);
  pos_start += 6;
  assert((pos_end = buf_str.find("\r", pos_start)) != string::npos);
  string host = buf_str.substr(pos_start, pos_end-pos_start);

  int range_start = -1, range_end = -1;
  parseRange(buf_str, &range_start, &range_end);
  LOG("start = " + to_string(range_start) + " end = " + to_string(range_end) +"\n");

  if(method == "GET") {
    ifstream t(("dir/" + filename).c_str());
    if(!t.good()) t.open(filename);
    HttpMsg msg;
    if(t.good()) {
      stringstream buffer;
      buffer << t.rdbuf();
      if(range_start == -1) {
        msg.status = 200;
        msg.status_msg = "OK";
        msg.body = buffer.str();
      } else {
        msg.status = 206;
        msg.status_msg = "Partial Content";
        msg.body = buffer.str().substr(range_start, min(buffer.str().length(), (size_t)range_end - range_start + 1));
      }
    } else {
      msg.status = 404;
      msg.status_msg = "Not Found";
    }
    string http_resp = makeHttpResponse(&msg);
    LOG( "(" + http_resp.substr(0, http_resp.length() - msg.body.length()) + ")\n");
    SSL_write(ssl, http_resp.c_str(), http_resp.length());
  }
}

SSL_CTX *create_context()
{
    const SSL_METHOD *method;
    SSL_CTX *ctx;

    method = TLS_server_method();

    ctx = SSL_CTX_new(method);
    if (!ctx) {
        perror("Unable to create SSL context");
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    return ctx;
}

void configure_context(SSL_CTX *ctx)
{
    /* Set the key and cert */
    if (SSL_CTX_use_certificate_file(ctx, "keys/cnlab.cert", SSL_FILETYPE_PEM) <= 0) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, "keys/cnlab.prikey", SSL_FILETYPE_PEM) <= 0 ) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }
}

void* https_server(void* args) {
  int httpsfd = socket(PF_INET, SOCK_STREAM, 0);
  assert(httpsfd != -1);
  sockaddr_in https_addr = {.sin_family = AF_INET, .sin_port = (in_port_t)htons(HPPTSPORT), .sin_addr = {.s_addr=htonl(INADDR_ANY)}};
  assert(bind(httpsfd, (struct sockaddr*)&https_addr, sizeof(https_addr)) != -1);
  assert(listen(httpsfd, 10) != -1);

  SSL_CTX *ctx;
  ctx = create_context();
  configure_context(ctx);

  while(1) {
    sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int clientfd = accept(httpsfd, (struct sockaddr*)&client_addr, &client_len);
    LOG("port=" + to_string(client_addr.sin_port) + " addr=" + to_string(client_addr.sin_addr.s_addr) + " fd=", to_string(clientfd) + "\n");
    assert(clientfd != -1);
    SSL* ssl = SSL_new(ctx);

    SSL_set_fd(ssl, clientfd);
    SSL_accept(ssl);
    requestHttps(ssl);
  }
  return 0;
}


int main() {
  pthread_t tids[2];
  pthread_create(&tids[0], NULL, http_server, NULL);
  pthread_create(&tids[1], NULL, https_server, NULL);
  while(1);

}