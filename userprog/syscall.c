#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "process.h"
#include <string.h>
#include "devices/shutdown.h"

int get_arg(struct intr_frame * f, int index);
int valid_ptr(int *ptr);
int valid_arg(struct intr_frame * f, int index);
static void syscall_handler (struct intr_frame *);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  int syscall_num;

  /* check f->frame_pointer validity */
  struct thread *t = thread_current();
  struct file_node *f_node;

  if( f->frame_pointer != NULL && is_user_vaddr( f->frame_pointer ) && valid_arg(f, 0) )
  {
    memcpy( &syscall_num, f->esp, sizeof(int));
    switch( syscall_num )
    {
      case SYS_HALT:
        shutdown_power_off();
        break;

      case SYS_EXIT:
        if (! valid_arg(f, 1))
	{
	  // if arg is invalid, exit with -1
	  t->return_status = -1;
	  f->eax = -1;
          thread_exit();  
	}
	else
	{
	  // arg is valid, return that value
          t->return_status = get_arg(f, 1);
	  f->eax = get_arg(f, 1);
	  thread_exit();
	}
	break;

      case SYS_EXEC:
        if (! valid_arg(f, 1) || ! valid_ptr(get_arg(f, 1)))
	{
	  // if arg is invalid, pid = -1
	  f->eax = -1;
          thread_exit();
	}
	else
	{
          // eax keeps the return value
          f->eax = process_execute((char *)get_arg(f, 1));

	}
	break;
      
      case SYS_WAIT:
        if (! valid_arg(f, 1))
	{
	  // if arg is invalid, pid = -1
	  f->eax = -1;
          thread_exit();  
	}
	else
	{
	  f->eax = process_wait((tid_t)get_arg(f, 1));
	}
	break;

      case SYS_CREATE:
        if (! valid_arg(f, 1) || ! valid_arg(f, 2) || ! valid_ptr(get_arg(f, 1)))
	{
	  // if arg is invalid, killed
	  f->eax = -1;
          thread_exit();  
	}
	else
	{
	  f->eax = filesys_create(get_arg(f, 1), get_arg(f, 2));
	}
        break;

      case SYS_REMOVE:
        if (! valid_arg(f, 1))
	{
	  // if arg is invalid, killed
	  f->eax = -1;
          thread_exit();  
	}
	else
	{
	  f->eax = filesys_remove(get_arg(f, 1));
	}
        break;

      case SYS_OPEN:
        if (! valid_arg(f, 1) || ! valid_ptr(get_arg(f, 1)))
	{
	  // if arg is invalid, killed
	  f->eax = -1;
          thread_exit();  
	}
	else
	{
	  f_node = calloc(1, sizeof (struct file_node));
	  ASSERT ( f_node != NULL );
	    
	  if ( (f_node->file = filesys_open( get_arg(f, 1)) ) == NULL)
	  {
	    f->eax = -1;
	  }
	  else
	  {
	    f_node->fd = t->fd_index;
	    t->fd_index++;
	    list_push_front( &t->file_list, &f_node->file_elem );
	    f->eax = f_node->fd;
	  }
	}
        break;

      case SYS_FILESIZE:
        if (! valid_arg(f, 1))
	{
	  // if arg is invalid, killed
	  f->eax = -1;
          thread_exit();  
	}
	else
	{
	  // get the f_node with given fd
	  f_node = get_file_node(get_arg(f, 1));

	  ASSERT (f_node != NULL);

	  f->eax = inode_length( file_get_inode(f_node->file) );
	}
        break;

      case SYS_READ:
        if (! valid_arg(f, 1) || ! valid_arg(f, 2) || ! valid_arg(f, 3) || ! valid_ptr(get_arg(f, 2)))
	{
	  f->eax = -1;
          thread_exit();  
	}
	else
	{
	  int fd = get_arg(f, 1);

	  if (fd == 0)
	  {
	    // input_getc();
	  }
	  else if (fd >= 2 && fd < t->fd_index)
	  {
	    f->eax = file_read(get_file_node(fd)->file, get_arg(f, 2), get_arg(f, 3));
	  }
	  else
	  {
	    f->eax = -1;
	  }
	}
	break;
       
      case SYS_WRITE:
        if (! valid_arg(f, 1) || ! valid_arg(f, 2) || ! valid_arg(f, 3) || ! valid_ptr(get_arg(f, 2)))
	{
	  f->eax = -1;
          thread_exit();  
	}
	else
	{
	  int fd = get_arg(f, 1);

	  if (fd == 1)
	  {
	    putbuf(get_arg(f, 2), get_arg(f, 3));
	  }
	  else if (fd >= 2 && fd < t->fd_index)
	  {
	    f->eax = file_write(get_file_node(fd)->file, get_arg(f, 2), get_arg(f, 3));
	  }
	  else
	  {
	    f->eax = 0;
	  }
	}
	break;

      case SYS_SEEK:
        if (! valid_arg(f, 1) || ! valid_arg(f, 2)  )
	{
	  f->eax = -1;
          thread_exit();  
	}
	else
	{
	  int fd = get_arg(f, 1);
	  if ( fd >= 2 && fd < t->fd_index )
	  {
	    f_node = get_file_node(fd);
	    file_seek( f_node->file , get_arg(f, 2) );
	  }
	}
        break;

      case SYS_TELL:
        if (! valid_arg(f, 1))
	{
	  f->eax = -1;
          thread_exit();  
	}
	else
	{
	  int fd = get_arg(f, 1);
	  if ( fd >=2 && fd < t->fd_index )
	  {
	    f_node = get_file_node(fd);
	    f->eax = file_tell( f_node->file ); 
	  }
	  else
	  {
	    f->eax = -1;
	  }
	}
        break;

      case SYS_CLOSE:
        if (! valid_arg(f, 1))
	{
	  f->eax = -1;
          thread_exit();  
	}
	else
	{
	  int fd = get_arg(f, 1);
	  if ( fd >=2 && fd < t->fd_index )
	  {
	    if ((f_node = get_file_node(fd)) != NULL)
	    {
	      file_close( f_node->file ); 
	      list_remove( &f_node->file_elem );
	    }
	    else
	    {
	      f->eax = -1;
	    }
	  }
	  else
	  {
	    f->eax = -1;
	  }
	}
        break;

      default:
        // TODO
	;
    }
  }
  else
  {
    thread_exit ();
  }
}

int
valid_ptr(int *ptr)
{
  return ptr != NULL && is_user_vaddr( ptr ) && ( pagedir_get_page(thread_current()->pagedir, ptr) != NULL );
}

/*
Check validity of ith argument.
They may exit after calling this.
*/
int
valid_arg(struct intr_frame * f, int index)
{
  int *ptr = (int *)(f->esp) + index; // current point (to stack)
  
  return valid_ptr(ptr);
}

/* get the ith argument above esp */
int
get_arg(struct intr_frame * f, int index)
{
  return * ( (int *)(f->esp) + index ); // current point (to stack)
}
