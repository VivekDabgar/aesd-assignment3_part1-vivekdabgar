#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

int main(int argc, char *argv[])
{
	//Step:1 Setup Syslog
	openlog(NULL, LOG_PID, LOG_USER);

	//step:2 check arguments
	if (argc !=3)
	{
		syslog(LOG_ERR, "Invalid number of arguments: %d. writer <file> <string>", argc-1);
		fprintf(stderr, "Usage: %s <writefile> <writestr>\n", argv[0]);
		closelog();
		return 1;
	}
	const char *writefile= argv[1];
	const char *writestr= argv[2];

	//Step :3 logging what we are doing:
	syslog(LOG_DEBUG, "Writing %s to %s", writestr, writefile);

	//Step:4 open file
	int fd= open(writefile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd == -1) {
		printf("File descriptor:%d", fd);
		syslog(LOG_ERR, "Failed to open file %s: %s", writefile, strerror(errno));
		fprintf(stderr, "Error: Could not open file %s: %s\n", writefile, strerror(errno));
		closelog();
		return 1;
	}

	//step:5 write the string
	if(write(fd, writestr, strlen(writestr)) == -1) {
		syslog(LOG_ERR, "Failed to write to a file %s: %s", writefile, strerror(errno));
		fprintf(stderr, "Error: Could not write to file %s\n", writefile);
		close(fd);
		closelog();
		return 1;
	}

	//step:6 Close file and syslog
	close(fd);
	closelog();

	return 0;
}
