#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sys/wait.h>
#include <stlib.h>

#define ISspace(x) isspace((int)(x))

#define SERVER_STRING "Server:jdbhttpd/0.1.0\r\n"

void accept_request(int);  //处理从套接字上监听到的一个HTTP请求
void bad_request(int);
void cat(int,FILE *);      //读取服务器上某个文件写到socket套接字
void cannot_execute(int);	//主要处理发生在执行cgi程序时出现的错误。
void error_die(const char *);  //把错误信息写到perror并退出
void execute_cgi(int,const char *,const char *,const char *);  
int get_line(int,char * ,int); //读取套接字的一行，把回车换行等情况都统一为换行符结束。
void headers(int,const char *); //把HTTP响应的头部写到套接字。
void not_found(int);
void serve_file(int,const char *); //调用cat把服务器文件返回给浏览器
int startup(u_short *);//初始化httpd服务，包括建立套接字，绑定端口，进行监听等。
void unimplemented(int);  //返回给浏览器表明收到的hTTP请求所用的method不被支持。


void accept_request(int client)
{
	char buf[1024];
	int numchars;
	char method[255];
	char url[255];
	char path[512];
	size_t i, j;
	struct stat st;
	int cgi = 0;
	
	char *query_string = NULL;
	
	/*得到请求的第一行*/
	numchars = get_line(client,buf, sizeof(buf));
	i = 0; j = 0;
	
	/*把客户的请求方法存到method数组*/
	while(!ISspace(buf[j]) && (i < sizeof(method)-1)){
		method[i] = buf[j];
		i++; j++;
	}
	method[i] = '\0';
	
	/*如果既不是GET又不是POST则无法处理*/
	if(strcasecmp(method,"GET") && strcasecmp(method,"POST"))
	{
	 unimplemented(client);
	 return;
	}
	
	/*POST的时候开启cgi*/
	if(strcasecmp(method,"POST") == 0)
		cgi = 1;
	
	/*读取url地址*/
	i = 0;
	while(ISspace(buf[j]) && (j< sizeof(buf)))
		j++;
	while(!ISspace(buf[j]) && (i<sizeof(url)-1) && (j<sizeof(buf)))
	{
		/*存下url*/
		url[i] = buf[j];
		i++; j++;
	}
	url[i] = '\0';
	
	/*处理GET方法*/
	if(strcasecmp(method, "GET") == 0)
	{
		query_string = url;
		while ((*query_string != '?') && (*query_string != '\0'))
			query_string++;
		if(*query_string == '?')
		{
			cgi = 1;
			*query_string = '\0';
			query_string++;
		}
	}
	
	/*格式化url到path数组，html文件都在htdocs中*/
	sprintf(path,"htdoc%s",url);
	/*默认情况下为index.html*/
	if(path[strlen(path)-1] == '/')
		strcat(path,"index.html");
	
	/*根据路径找到对应文件*/
	if(stat(path,&st) == -1)
	{
		/*把所有headers的信息都丢弃*/
		while((numchars)>0 && strcmp("\n",buf))
			numchars = get_line(client,buf,sizeof(buf));
		not_found(client);
	}
	else
	{
		/*如果是个目录，则默认使用该目录下index.html文件*/
		if((st.st_mode & S_IFMT) == S_IFDIR)
			strcat(path,"/index.html");
		if((st.st_mode & S_IXUSR) ||
		   (st.st_mode & S_IXGRP) ||
		   (st.st_mode & S_IXOTH) )
		   cgi = 1;
		   
		   /*不是cgi,直接把服务器文件返回，否则执行cgi*/
		if(!cgi)
			serve_file(client,path);
		else
			execute_cgi(client,path,method,query_string);
	}
	
	/*断开与客户端的链接（http 特点：无连接）*/
	close(client);
}

/*
如果请求有问题，通知客户
参数：客户socket
*/
void bad_request(int client)
{
	char buf[1024];
	
	sprintf(buf,"HTTP/1.0 400 BAD REQUEST\r\n");
	send(client,buf,sizeof(buf),0);
	sprintf(buf,"Content-type:text/html\r\n");
	send(client,buf,sizeof(buf),0);
	sprintf(buf,"\r\n");
	send(client,buf,sizeof(buf),0);
	sprintf(buf,"<p>Your browser sent a bad request,");
	send(client,buf,sizeof(buf),0);
	sprintf(buf,"such as a POST without a Content-Length. \r\n");
	send(client,buf,sizeof(buf),0);
}

/*
把一个文件的整个内容放到socket上；
你这个函数根据unix的cat命令命名，
因为，它可能更容易做例如pipe,fork ，exec("cat") 等这样的事情
参数：client socket描述符
	 指向文件的指针
*/
void  cat(int client FILE *resource)
{
	char buf[1024];
	
	/*读取文件中的所有数据到socket*/
	fgets(buf,sizeof(buf),resource);
	while(!feof(resource)){
		send(client,buf,strlen(buf),0);
		fgets(buf,sizeof(buf),resource);
	}
}

/*
	如果一个cgi脚本不能够被执行时，通知client
	参数：client socket描述符
*/
void cannot_execute(int client)
{
	char buf[1024];
	
	sprintf(buf,"HTTP/1.0 500 Internal Server Error\r\n");
	send(client,buf,strlen(buf),0);
	sprintf(buf,"Content-type:text/html\r\n");
	send(client,buf,strlen(buf),0);
	sprintf(buf,"\r\n");
	send(client,buf,strlen(buf),0);
	sprintf(buf,"<p>Error prohibited CGI execution.\r\n");
	send(client,buf,strlen(buf),0);
}

/*
打印一个错误信息，并且退出程序
*/
void error_die(const char *sc)
{
	perror(sc);
	exit(1);
}

/*
	执行一个cgi脚本。这个脚本将需要去设置合适的环境变量
	参数： client socket 描述符
		   cgi脚本的路径
*/
void execute_cgi(int client,const char *path,
				const char *method,const char *query_string)
{
	char buf[1024];
	int cgi_output[2];
	int cgi_input[2];
	pid_t pid;
	int status;
	int i;
	char c;
	int numchars = 1;
	int content_length = -1;
	
	buf[0] = 'A'; buf[1] = '\0';
	if(strcasecmp(method,"GET") == 0){
		/*把所有的HTTP header读取并丢弃*/
		while((numchars>0) && strcmp("\n",buf))
			numchars = get_line(client,buf,sizeof(buf));
	}
	else   //POST
	{
		numchars = get_line(client,buf,sizeof(buf));
		while((numchars)>0 && strcmp("\n",buf))
		{
			/*利用\0进行分隔*/
			buf[15] = '\0';
			/*HTTP请求的特点*/
			if(strcasecmp(buf,"Content-Length:")== 0)
				content_length = atoi(&(buf[16]));
			numchars = get_line(client,buf,sizeof(buf));
		}
		/*没有找到content_length*/
		if(content_length == -1){
			bad_request(client);
			return;
		}
	}
	
	/*正确，HTTP状态码 200*/
	sprintf(buf,"HTTP/1.0 200 OK\r\n");
	send(client,buf,strlen(buf),0);
	
	/*建立管道*/
	if(pipe(cgi_output)<0){
		cannot_execute(client);
		return;
	}
	/*建立管道*/
	if(pipe(cgi_input)<0){
		cannot_execute(client);
		return ;
	}
	
	if((pid = fork())<0){
		cannot_execute(client);
		return;
	}
	
	if(pid == 0){
		char meth_env[255];
		char query_env[255];
		char length_env[255];
		
		/*把STDOUT重定向到cgi_output的写入端*/
		dup2(cgi_output[1],1);
		/*把STDIN重定向到cgi_input的读取端*/
		dup2(cgi_input[0],0);
		
		/*关闭cgi_input的写入端和cgi_output的读取端*/
		close(cgi_output[0]);
		close(cgi_output[1]);
		/*设置request_method的环境变量*/
		sprintf(meth_env,"REQUEST_METHOD=%s",method);
		putenv(meth_env);
		if(strcasecmp(method,"GET")==0){
			/*设置query_string的环境变量*/
			sprintf(query_env,"QUERY_STRING=%s",query_string);
			putenv(query_env);
		}else{ /*POST*/
			/*设置content_length的环境变量*/
			sprintf(length_env,"CONTENT_LENGTH=%d",content_length);
			putenv(length_env);
		}
		
		/*用execl运行cgi程序*/
		execl(path,path,NULL);
		exit(0);
	}else{
		/*关闭cgi_input的读取端和cgi_output的写入端*/
		close(cgi_output[1]);
		close(cgi_input[0]);
		if(strcasecmp(method,"POST") == 0)
			/*接收POST过来的数据*/
			for(i=0;i<content_length;i++){
				recv(client,&c,1,0);
				/*把POST数据写入cgi_input，现在重定向到STDIN*/
				write(cgi_input[1],&c,1);
			}
			
			/*读取cgi_output的管道输出到客户端，该管道输入是STDOUT*/
			while(read(cgi_output[0],&c,1)>0)
				send(client,&c,1,0);
			
			close(cgi_output[0]);
			close(cgi_input[1]);
			/*等待子进程*/
			waitpid(pid,&status,0);
	}
}

/*
	从一个socket获取一行，。。。
	
	参数：socket描述符
		buffer 保存数据的缓冲区
		size   缓冲区的大小
*/
int get_line(int sock,char *buf,int size)
{
	int i=0;
	char c = '\0';
	int n;
	
	/*把终止条件统一为\n换行符，标准化buf数组*/
	while((i<size-1)&& (c != '\n'))
	{
		/*从sock中一次读一个字符，循环读*/
		n = recv(sock,&c,1,0);
		if(n>0){
			
			/*收到\r则继续接收下个字节，因为换行符可能是\r\n */
			if(c == '\r')
			{
				/*使用MSG_PEEK 标志使下一次读取依然可以得到这次读取的内容，*/
				n = recv(sock,&c,1,MSG_PEEK);
				/*但如果是换行符则把它吸收掉*/
				if((n>0) && (c == '\n'))
					recv(sock,&c,1,0);
				else
					c = '\n';
			}
			buf[i] = c;
			i++;
		}
		else
			c='\n';
	}
	buf[i] = '\0';
	
	/*返回读取的字符数*/
	return(i);
}

/*
返回一个文件的http 头信息
参数：socket
	文件名
*/
void headers(int client,const char *filename)
{
	char buf[1024];
	(void)filename;  //定义文件类型
	
	strcpy(buf,"HTTP/1.0 200 OK\r\n");
	send(client,buf,strlen(buf),0);
	/*服务器信息*/
	strcpy(buf,SERVER_STRING);
	send(client,buf,strlen(buf),0);
	sprintf(buf,"Content-Type:text/html\r\n");
	send(client,buf,strlen(buf),0);
	strcpy(buf,"\r\n");
	send(client,buf,strlen(buf),0);
}

/*
返回一个404信息给客户端
*/
void not_found(int client)
{
	char buf[1024];
	
	sprintf(buf,"HTTP/1.0 404 NOT FOUND\r\n");
	send(client,buf,strlen(buf),0);
	 sprintf(buf, SERVER_STRING);
	 send(client, buf, strlen(buf), 0);
	 sprintf(buf, "Content-Type: text/html\r\n");
	 send(client, buf, strlen(buf), 0);
	 sprintf(buf, "\r\n");
	 send(client, buf, strlen(buf), 0);
	 sprintf(buf, "<HTML><TITLE>Not Found</TITLE>\r\n");
	 send(client, buf, strlen(buf), 0);
	 sprintf(buf, "<BODY><P>The server could not fulfill\r\n");
	 send(client, buf, strlen(buf), 0);
	 sprintf(buf, "your request because the resource specified\r\n");
	 send(client, buf, strlen(buf), 0);
	 sprintf(buf, "is unavailable or nonexistent.\r\n");
	 send(client, buf, strlen(buf), 0);
	 sprintf(buf, "</BODY></HTML>\r\n");
	 send(client, buf, strlen(buf), 0);
}

/*
	发送client一个常规文件，使用头，如果一个错误发生要报告。
	参数： 
*/
	void serve_file(int client,const char *filename)
	{
		FILE *resource = NULL;
		int numchars = 1;
		char buf[1024];
		
		/*读取并丢弃 header*/
		buf[0] = 'A';buf[1] = '\0';
		while((numchars>0) && strcmp("\n",buf))
			numchars = get_line(client,buf,sizeof(buf));
		resource = fopen(filename,"r");
		if(resource == NULL)
			not_found(client);
		else{
			/*写 HTTP header*/
			headers(client,filename);
			cat(client,resource)
		}
		fclose(resource);
	}

/*
在指定的端口开启一个监听web 链接的线程；
如果端口是0，。。。

参数：一个指针变量，指向的变量存放端口。
返回：socket
*/
int startup(u_short *port)
{
	int httpd = 0;
	struct sockaddr_in name;
	
	httpd = socket(PF_INET,SOCK_STREAM,0);
	if(httpd == -1)
	{
		error_die("socket");
	}
	memset(&name,0,sizeof(name)); //也可以用bzero
	name.sin_family = AF_INET;
	name.sin_port = htons(*port);
	name.sin_addr.s_addr = htonl(INADDR_ANY); //任何网络接口
	if(bind(httpd,(struct sockaddr *)&name,sizeof(name))<0)
		error_die("bind");
	
	/*如果当前指定端口是0，则动态分配一个端口*/
	if(*port == 0)
	{
		int namelen = sizeof(name);
		if(getsockname(httpd,(struct sockaddr *)&name,&namelen) == -1)
			error_die("getsockname");
		*port = ntohs(name.sin_port); //系统动态分配一个端口号
	}
	
	/*开始监听*/
	if(listen(httpd,5)<0)
		error_die("listen");
	
	/*返回socket id*/
	return(httpd);  //返回服务套接字描述符
}

/*
	如果web请求的方法还没有实现时，通知客户
	参数： client socket
*/
void unimplemented(int client)
{
	char buf[1024];
	sprintf(buf,"HTTP/1.0 501 Method Not Implemented\r\n");
	send(client,buf,strlen(buf),0);
	sprintf(buf, SERVER_STRING);
	 send(client, buf, strlen(buf), 0);
	 sprintf(buf, "Content-Type: text/html\r\n");
	 send(client, buf, strlen(buf), 0);
	 sprintf(buf, "\r\n");
	 send(client, buf, strlen(buf), 0);
	 sprintf(buf, "<HTML><HEAD><TITLE>Method Not Implemented\r\n");
	 send(client, buf, strlen(buf), 0);
	 sprintf(buf, "</TITLE></HEAD>\r\n");
	 send(client, buf, strlen(buf), 0);
	 sprintf(buf, "<BODY><P>HTTP request method not supported.\r\n");
	 send(client, buf, strlen(buf), 0);
	 sprintf(buf, "</BODY></HTML>\r\n");
	 send(client, buf, strlen(buf), 0);
}

int main(void)
{
	int server_sock = -1;
	u_short port = 0;
	int client_sock = -1;
	struct sockaddr_in client_name;
	int client_name_len = sizeof(client_name);
	pthread_t newthread;
	
	server_sock = startup(&port);
	printf("httpd running on port %d\n",port);
	
	while(1)
	{
		/*套接字收到客户端连接请求*/
		client_sock = accept(server_sock,(struct sockaddr *)&client_name,&client_name_len);
		if(client_sock == -1)
			error_die("accept");
		/*派生新线程用accept_request函数处理新请求*/
		if(pthread_create(&newthread,NULL,accept_request,client_sock) != 0)
			perror("pthread_create");
	}
	
	close(server_sock);
	
	return(0);
}

/*
	说明：源码htdocs目录下的cgi都是perl写的
	gdb
*/
