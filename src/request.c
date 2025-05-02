#include "io_helper.h"
#include "request.h"
#include <pthread.h>
#include <stdlib.h>

#define MAXBUF (8192)

int num_threads = DEFAULT_THREADS;
int buffer_max_size = DEFAULT_BUFFER_SIZE;
int scheduling_algo = DEFAULT_SCHED_ALGO;

//
//	TODO: add code to create and manage the buffer
//

// Make a structure for the requests
typedef struct {
	int fd;
	int filesize;
	char filename[MAXBUF];
} request;

// Make a structure for request buffers
typedef struct {
	request buffer[DEFAULT_BUFFER_SIZE];
	int count;
	pthread_mutex_t lock;
	pthread_cond_t not_empty;
	pthread_cond_t not_full;
} request_buffer;

request_buffer request_buf;

// Initialize the buffer
void buffer_init() {
                request_buf.count = 0;
                pthread_mutex_init(&request_buf.lock, NULL);
                pthread_cond_init(&request_buf.not_empty, NULL);
                pthread_cond_init(&request_buf.not_full, NULL);
}

//
// Sends out HTTP response in case of errors
//
void request_error(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXBUF], body[MAXBUF];
    
    // Create the body of error message first (have to know its length for header)
    sprintf(body, ""
	    "<!doctype html>\r\n"
	    "<head>\r\n"
	    "  <title>CYB-3053 WebServer Error</title>\r\n"
	    "</head>\r\n"
	    "<body>\r\n"
	    "  <h2>%s: %s</h2>\r\n" 
	    "  <p>%s: %s</p>\r\n"
	    "</body>\r\n"
	    "</html>\r\n", errnum, shortmsg, longmsg, cause);
    
    // Write out the header information for this response
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    write_or_die(fd, buf, strlen(buf));
    
    sprintf(buf, "Content-Type: text/html\r\n");
    write_or_die(fd, buf, strlen(buf));
    
    sprintf(buf, "Content-Length: %lu\r\n\r\n", strlen(body));
    write_or_die(fd, buf, strlen(buf));
    
    // Write out the body last
    write_or_die(fd, body, strlen(body));
    
    // close the socket connection
    close_or_die(fd);
}

//
// Reads and discards everything up to an empty text line
//
void request_read_headers(int fd) {
    char buf[MAXBUF];
    
    readline_or_die(fd, buf, MAXBUF);
    while (strcmp(buf, "\r\n")) {
		readline_or_die(fd, buf, MAXBUF);
    }
    return;
}

//
// Return 1 if static, 0 if dynamic content (executable file)
// Calculates filename (and cgiargs, for dynamic) from uri
//
int request_parse_uri(char *uri, char *filename, char *cgiargs) {
    char *ptr;
    
    if (!strstr(uri, "cgi")) { 
	// static
	strcpy(cgiargs, "");
	sprintf(filename, ".%s", uri);
	if (uri[strlen(uri)-1] == '/') {
	    strcat(filename, "index.html");
	}
	return 1;
    } else { 
	// dynamic
	ptr = index(uri, '?');
	if (ptr) {
	    strcpy(cgiargs, ptr+1);
	    *ptr = '\0';
	} else {
	    strcpy(cgiargs, "");
	}
	sprintf(filename, ".%s", uri);
	return 0;
    }
}

//
// Fills in the filetype given the filename
//
void request_get_filetype(char *filename, char *filetype) {
    if (strstr(filename, ".html")) 
		strcpy(filetype, "text/html");
    else if (strstr(filename, ".gif")) 
		strcpy(filetype, "image/gif");
    else if (strstr(filename, ".jpg")) 
		strcpy(filetype, "image/jpeg");
    else 
		strcpy(filetype, "text/plain");
}

//
// Handles requests for static content
//
void request_serve_static(int fd, char *filename, int filesize) {
    int srcfd;
    char *srcp, filetype[MAXBUF], buf[MAXBUF];
    
    request_get_filetype(filename, filetype);
    srcfd = open_or_die(filename, O_RDONLY, 0);
    
    // Rather than call read() to read the file into memory, 
    // which would require that we allocate a buffer, we memory-map the file
    srcp = mmap_or_die(0, filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
    close_or_die(srcfd);
    
    // put together response
    sprintf(buf, ""
	    "HTTP/1.0 200 OK\r\n"
	    "Server: OSTEP WebServer\r\n"
	    "Content-Length: %d\r\n"
	    "Content-Type: %s\r\n\r\n", 
	    filesize, filetype);
       
    write_or_die(fd, buf, strlen(buf));
    
    //  Writes out to the client socket the memory-mapped file 
    write_or_die(fd, srcp, filesize);
    munmap_or_die(srcp, filesize);
}

//
// Fetches the requests from the buffer and handles them (thread logic)
//
void* thread_request_serve_static(void* arg)
{

	// TODO: write code to actualy respond to HTTP requests
	// Initialize buffer only once with the first thread
    	static pthread_once_t once = PTHREAD_ONCE_INIT;
    	pthread_once(&once, buffer_init);

    	while (1) {
            	pthread_mutex_lock(&request_buf.lock);
            	// Wait if the buffer is empty
            	while (request_buf.count == 0) {
			printf("Buffer count is 0, waiting on another thread");
                    	pthread_cond_wait(&request_buf.not_empty, &request_buf.lock);
            	}

		while (request_buf.count < num_threads) {
			pthread_mutex_unlock(&request_buf.lock);
			usleep(1000);
			pthread_mutex_lock(&request_buf.lock);
		}

            	// This will handle FIFO since it will go in order
            	int idx = 0;

            	// Handle the SFF algorithm (smallest file first)
            	if (scheduling_algo == 1) {
                    	printf("Got to the if statement");
			int min_size = request_buf.buffer[0].filesize;
                    	for (int i = 1; i < request_buf.count; i++) {
				printf("Got to the for statement");
                            	if (request_buf.buffer[i].filesize < min_size) {
                                    	min_size = request_buf.buffer[i].filesize;
                                    	printf("min_size = %d bytes\n", min_size);
					idx = i;
                            	}
                    	}
            	// Handle the random algorithm (picking a random request)
            	} else if (scheduling_algo == 2) {
			if (request_buf.count <= 0) {
				continue;
			}
                    	idx = rand() % request_buf.count;
         	}
            	// Handle request
            	request req = request_buf.buffer[idx];
            	for (int i = idx; i < request_buf.count - 1; i++) {
                    	request_buf.buffer[i] = request_buf.buffer[i + 1];
            	}
            	request_buf.count--;
		num_threads--;
		printf("Request is handled. Subtracting from request_buf count. It is now %d\n", request_buf.count);

            	// Signal if blocked
            	pthread_cond_signal(&request_buf.not_full);
            	pthread_mutex_unlock(&request_buf.lock);

		printf("Thread %ld serving: %s (%d bytes)\n", pthread_self(), req.filename, req.filesize);

            	// Write response then close
            	request_serve_static(req.fd, req.filename, req.filesize);
            	close_or_die(req.fd);
    	}
}

//
// Initial handling of the request
//
void request_handle(int fd) {
    int is_static;
    struct stat sbuf;
    char buf[MAXBUF], method[MAXBUF], uri[MAXBUF], version[MAXBUF];
    char filename[MAXBUF], cgiargs[MAXBUF];
    
	// get the request type, file path and HTTP version
    readline_or_die(fd, buf, MAXBUF);
    sscanf(buf, "%s %s %s", method, uri, version);
    printf("method:%s uri:%s version:%s\n", method, uri, version);

	// verify if the request type is GET or not
    if (strcasecmp(method, "GET")) {
		request_error(fd, method, "501", "Not Implemented", "server does not implement this method");
		return;
    }
    request_read_headers(fd);
    
	// check requested content type (static/dynamic)
    is_static = request_parse_uri(uri, filename, cgiargs);
    
	// get some data regarding the requested file, also check if requested file is present on server
    if (stat(filename, &sbuf) < 0) {
		request_error(fd, filename, "404", "Not found", "server could not find this file");
		return;
    }
    
	// verify if requested content is static
    if (is_static) {
		if (!(S_ISREG(sbuf.st_mode)) || !(S_IRUSR & sbuf.st_mode)) {
			request_error(fd, filename, "403", "Forbidden", "server could not read this file");
			return;
		}
		
		// TODO: write code to add HTTP requests in the buffer based on the scheduling policy

		pthread_mutex_lock(&request_buf.lock);

		// Wait if the buffer is full
		while (request_buf.count == DEFAULT_BUFFER_SIZE) {
			pthread_cond_wait(&request_buf.not_full, &request_buf.lock);
		}

		// Store information for request
		request *req = &request_buf.buffer[request_buf.count];
		req->fd = fd;
		req->filesize = sbuf.st_size;
		strcpy(req->filename, filename);
		request_buf.count++;
		printf("Adding to request_buf count. It is now %d\n", request_buf.count);

		pthread_cond_signal(&request_buf.not_empty);
		pthread_mutex_unlock(&request_buf.lock);

    } else {
		request_error(fd, filename, "501", "Not Implemented", "server does not serve dynamic content request");
    }
}
