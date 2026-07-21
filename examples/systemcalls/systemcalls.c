#include "systemcalls.h"
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>


/**
 * @param cmd the command to execute with system()
 * @return true if the command in @param cmd was executed
 *   successfully using the system() call, false if an error occurred,
 *   either in invocation of the system() call, or if a non-zero return
 *   value was returned by the command issued in @param cmd.
*/
bool do_system(const char *cmd)
{

   int retval;
   bool success = false;
   
   retval = system(cmd);
   
   if (retval == -1)
   {
     // system call failed so do some logging
      success = false;
     
   } 
   else if (WIFEXITED(retval) && WEXITSTATUS(retval) == 0)
   {
      success = true;
   }
   else
   {
      success = false;
      perror("failed: examples/systemcalls/systemcalls.c/do_system() command ");
   }

   return success;
}

/**
* @param count -The numbers of variables passed to the function. The variables are command to execute.
*   followed by arguments to pass to the command
*   Since exec() does not perform path expansion, the command to execute needs
*   to be an absolute path.
* @param ... - A list of 1 or more arguments after the @param count argument.
*   The first is always the full path to the command to execute with execv()
*   The remaining arguments are a list of arguments to pass to the command in execv()
* @return true if the command @param ... with arguments @param arguments were executed successfully
*   using the execv() call, false if an error occurred, either in invocation of the
*   fork, waitpid, or execv() command, or if a non-zero return value was returned
*   by the command issued in @param arguments with the specified arguments.
*/

bool do_exec(int count, ...)
{
    va_list args;
    va_start(args, count);
    char * command[count+1];
    int i;
    for(i=0; i<count; i++)
    {
        command[i] = va_arg(args, char *);
    }
    command[count] = NULL;
    // this line is to avoid a compile warning before your implementation is complete
    // and may be removed
    command[count] = command[count];

    va_end(args);
	
    fflush(stdout);
    pid_t child_pid;
    child_pid = fork();
    if (child_pid == -1)
    {
		
    	perror("fork failed");
        return false;
    }
    else if(child_pid == 0)
    {
        int ret = execv(command[0], command);
        if (ret == -1)
        {
        	perror("execv returned");
        	_exit(1);
     	}
    }
    else
    {
        int childstatus = 0;   
        if(waitpid(child_pid, &childstatus, 0) == -1)
        {
      		perror("waitpid");
            return false;
        }
      
        return ( WIFEXITED(childstatus) && WEXITSTATUS(childstatus) == 0); 
    }
	
    return true;
}

/**
* @param outputfile - The full path to the file to write with command output.
*   This file will be closed at completion of the function call.
* All other parameters, see do_exec above
*/
bool do_exec_redirect(const char *outputfile, int count, ...)
{
    va_list args;
    va_start(args, count);
    char * command[count+1];
    int i;
    for(i=0; i<count; i++)
    {
        command[i] = va_arg(args, char *);
    }
    command[count] = NULL;
    // this line is to avoid a compile warning before your implementation is complete
    // and may be removed
    command[count] = command[count];

    va_end(args);
    fflush(stdout);
    pid_t child_pid = fork();
    if (child_pid == -1)
    {
    	perror("fork");
    	return false;
    }
    else if (child_pid == 0)
    {
        int fd = open(outputfile, O_WRONLY | O_TRUNC | O_CREAT, 0644);

    	if (fd < 0)
    	{
        	perror(" open unable to open file ");
        	_exit(1);
        }

        if (dup2(fd, STDOUT_FILENO) < 0)
        {
        	perror("dup2");
      		close(fd);
        	_exit(1);
		}

        close(fd);

        execv(command[0], command);
        perror("execv");
        _exit(1);
    }
	
	else
	{
    	int childstatus = 0;
    	if (waitpid(child_pid, &childstatus, 0) == -1)
    	{
        	perror("waitpid");
        	return false;
        }
        return (WIFEXITED(childstatus) && WEXITSTATUS(childstatus) == 0);
	}	
}
