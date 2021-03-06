#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
#define DIRECT_BLOCK 124
#define MAX_BLOCK_NUMBER 16637 // 1 + 124 + 2 ** 7 + 2 ** 14

/* MODIFIED: return the total number of sectors needed to save SECTORS's data */
size_t compute_total_sectors (size_t sectors);

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t sectors[DIRECT_BLOCK];		/* MODIFIED Sectors pointing to direct or indirect data block. */
    block_sector_t ib;
    bool is_directory;			/* MODIFIED */
    char padding[3];
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
  };

/*the struct of indirect_block which has 128 pointers pointing to second level ib */
struct indirect_block
{
  block_sector_t sectors[128];
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
  };

/* Returns the block device sector that contains byte offset POS
   within INODE.

   MODIFIED traverse

   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  block_sector_t index, first_ib_index, second_ib_index;
  struct indirect_block *first_ib, *second_ib;

  if (pos < inode->data.length)
  {
    index = pos / BLOCK_SECTOR_SIZE;

    // direct block
    if (index < DIRECT_BLOCK)
    {
      return inode->data.sectors[index];
    }
    else
    {
      // number of entry in first level ib
      first_ib_index = (index - DIRECT_BLOCK) / 128;
      second_ib_index = (index - DIRECT_BLOCK) % 128;

      // read first level ib
      block_read (fs_device, inode->data.ib, first_ib);

      // read second level ib
      block_read (fs_device, first_ib->sectors[first_ib_index], second_ib);

      return second_ib->sectors[second_ib_index];
    }
  }
  else
    return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = NULL;
  block_sector_t total_sectors;
  bool success = false;
  
  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      total_sectors = compute_total_sectors (sectors);
      if (total_sectors > MAX_BLOCK_NUMBER)
      {
        // file too large, counting indirect blocks
        return false;
      }

      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;

      /* create direct / indirect data blocks */
      if (free_map_unused () >= total_sectors + 1) 
        {
	  write_sectors_to_disk (total_sectors, disk_inode);

          block_write (fs_device, sector, disk_inode);
          
          success = true; 
        } 
      free (disk_inode);
    }

  printf ("inode_create: length is %d\n", length);

  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  block_read (fs_device, inode->sector, &inode->data);

  printf ("inode_open: length is %d\n", inode->data.length);
  
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  block_sector_t index, ib, i, k, r;
  struct indirect_block *first_ib, *second_ib;

  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
	    index = inode->data.length / BLOCK_SECTOR_SIZE;

	    // direct block
	    for (i = 0; i < DIRECT_BLOCK && i < index; i++)
	    {
	      free_map_release(inode->data.sectors[i], 1);
	    }
	    if ( i < index )
	    {
	      // number of entry in first level ib
	      ib = inode->data.ib;
	      block_read (fs_device, ib, first_ib);
	      k = 0;
              while( i < index )
	      {
	        block_read (fs_device, first_ib->sectors[k], second_ib);
	        r = 0;
		while( i < index && r < 128 )
		{
		  free_map_release(second_ib->sectors[r], 1);
		  r++;
		  i++;
		}
		free_map_release(first_ib->sectors[k], 1);
		k++;
	      }
	    }
	  // free inode
          free_map_release (inode->sector, 1);
        }

      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          block_read (fs_device, sector_idx, buffer + bytes_read);
        }
      else 
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          block_read (fs_device, sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          block_write (fs_device, sector_idx, buffer + bytes_written);
        }
      else 
        {
          /* We need a bounce buffer. */
          if (bounce == NULL) 
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left) 
            block_read (fs_device, sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          block_write (fs_device, sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}

/* For SECTORS number of direct sectors, return 
number of sectors needed (counting indirect ones)
not include the sector inode needs*/
size_t
compute_total_sectors (size_t sectors)
{
  int count;
  if( sectors <= DIRECT_BLOCK )
  {
    return sectors;
  }
  else
  {
    int second_level = ( sectors - DIRECT_BLOCK ) / 128;
    int indirect_sectors = sectors - DIRECT_BLOCK;
    // Direct data + Indirect data + indirect IB (1st level: 1, 2nd level: second_level)
    return indirect_sectors + second_level + 1 + DIRECT_BLOCK;
  }
}

/* Create secotrs in disk with 2 level indirected block. */
void
write_sectors_to_disk( block_sector_t total_sectors, struct inode_disk *disk_inode )
{
  block_sector_t position;
  int i, k;

  // data in direct data blocks
  for( i = 0; i < total_sectors && i < DIRECT_BLOCK; i++ )
  {
    free_map_allocate(1, &position );
    disk_inode->sectors[i] = position;
  }

  // data in IB
  if( i < total_sectors )
  {
    // get the first level IB's sector position and save it into inode->ib
    struct indirect_block *first_level = calloc(1, sizeof (struct indirect_block));
    free_map_allocate(1, &position );
    disk_inode->ib = position;
    i++;

    // create the first_level IB
    int ib_index = 0;
    while ( i < total_sectors )
    {
      ASSERT (ib_index < 128);

      struct indirect_block *second_level = calloc(1, sizeof (struct indirect_block));
      free_map_allocate(1, &position );
      first_level->sectors[ib_index] = position;
      i++;
   
      // create the second_level IB
      for( k = 0; (k < 128) && (i + k < total_sectors); k++ )
      {
        free_map_allocate(1, &position );
        second_level->sectors[k] = position;
      } 

      i = i + k;

      //write the second_level IB to disk
      block_write (fs_device, first_level->sectors[ib_index], second_level);
      free( second_level );
      ib_index++;
    }

    //write the first_level IB to disk
    block_write (fs_device, disk_inode->ib, first_level);
    free( first_level );
  }
}
