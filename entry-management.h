/*
  2010, 2011 Stef Bon <stefbon@gmail.com>

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#ifndef ENTRY_MANAGEMENT_H
#define ENTRY_MANAGEMENT_H

struct cdfs_inode_struct {
    fuse_ino_t ino;
    uint64_t nlookup;
    struct cdfs_inode_struct *id_next;
    struct cdfs_entry_struct *alias;
};


struct cdfs_entry_struct {
    char *name;
    struct cdfs_inode_struct *inode;
    struct cdfs_entry_struct *name_next;
    struct cdfs_entry_struct *name_prev;
    struct cdfs_entry_struct *parent;
    size_t namehash;
    int hide;
    struct stat cached_stat;
    void *data;
    unsigned char cached;
    unsigned char type;
};


// Prototypes

int init_hashtables();
void add_to_inode_hash_table(struct cdfs_inode_struct *inode);
void add_to_name_hash_table(struct cdfs_entry_struct *entry);
void remove_entry_from_name_hash(struct cdfs_entry_struct *entry);
struct cdfs_inode_struct *find_inode(fuse_ino_t inode);
struct cdfs_entry_struct *find_entry(fuse_ino_t parent, const char *name);
struct cdfs_entry_struct *create_entry(struct cdfs_entry_struct *parent, const char *name, struct cdfs_inode_struct *inode);
void remove_entry(struct cdfs_entry_struct *entry);
void assign_inode(struct cdfs_entry_struct *entry);
struct cdfs_entry_struct *new_entry(fuse_ino_t parent, const char *name);


#endif
