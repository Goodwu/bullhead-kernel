/*
 * fs/f2fs/namei.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/fs.h>
#include <linux/f2fs_fs.h>
#include <linux/pagemap.h>
#include <linux/sched.h>
#include <linux/ctype.h>

#include "f2fs.h"
#include "node.h"
#include "xattr.h"
#include "acl.h"
#include <trace/events/f2fs.h>

static struct inode *f2fs_new_inode(struct inode *dir, umode_t mode)
{
	struct super_block *sb = dir->i_sb;
	struct f2fs_sb_info *sbi = F2FS_SB(sb);
	nid_t ino;
	struct inode *inode;
	bool nid_free = false;
	int err, ilock;

	inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	ilock = mutex_lock_op(sbi);
	if (!alloc_nid(sbi, &ino)) {
		mutex_unlock_op(sbi, ilock);
		err = -ENOSPC;
		goto fail;
	}
	mutex_unlock_op(sbi, ilock);

	inode->i_uid = current_fsuid();

	if (dir->i_mode & S_ISGID) {
		inode->i_gid = dir->i_gid;
		if (S_ISDIR(mode))
			mode |= S_ISGID;
	} else {
		inode->i_gid = current_fsgid();
	}

	inode->i_ino = ino;
	inode->i_mode = mode;
	inode->i_blocks = 0;
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	inode->i_generation = sbi->s_next_generation++;

	err = insert_inode_locked(inode);
	if (err) {
		err = -EINVAL;
		nid_free = true;
		goto out;
	}
	trace_f2fs_new_inode(inode, 0);
	mark_inode_dirty(inode);
	return inode;

out:
	clear_nlink(inode);
	unlock_new_inode(inode);
fail:
	trace_f2fs_new_inode(inode, err);
	make_bad_inode(inode);
	iput(inode);
	if (nid_free)
		alloc_nid_failed(sbi, ino);
	return ERR_PTR(err);
}

static int is_multimedia_file(const unsigned char *s, const char *sub)
{
	size_t slen = strlen(s);
	size_t sublen = strlen(sub);
	int ret;

	if (sublen > slen)
		return 0;

	ret = memcmp(s + slen - sublen, sub, sublen);
	if (ret) {	/* compare upper case */
		int i;
		char upper_sub[8];
		for (i = 0; i < sublen && i < sizeof(upper_sub); i++)
			upper_sub[i] = toupper(sub[i]);
		return !memcmp(s + slen - sublen, upper_sub, sublen);
	}

	return !ret;
}

/*
 * Set multimedia files as cold files for hot/cold data separation
 */
static inline void set_cold_files(struct f2fs_sb_info *sbi, struct inode *inode,
		const unsigned char *name)
{
	int i;
	__u8 (*extlist)[8] = sbi->raw_super->extension_list;

	int count = le32_to_cpu(sbi->raw_super->extension_count);
	for (i = 0; i < count; i++) {
		if (is_multimedia_file(name, extlist[i])) {
			set_cold_file(inode);
			break;
		}
	}
}

static int f2fs_create(struct inode *dir, struct dentry *dentry, umode_t mode,
						bool excl)
{
	struct super_block *sb = dir->i_sb;
	struct f2fs_sb_info *sbi = F2FS_SB(sb);
	struct inode *inode;
	nid_t ino = 0;
	int err, ilock;

	f2fs_balance_fs(sbi);

	inode = f2fs_new_inode(dir, mode);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	if (!test_opt(sbi, DISABLE_EXT_IDENTIFY))
		set_cold_files(sbi, inode, dentry->d_name.name);

	inode->i_op = &f2fs_file_inode_operations;
	inode->i_fop = &f2fs_file_operations;
	inode->i_mapping->a_ops = &f2fs_dblock_aops;
	ino = inode->i_ino;

	ilock = mutex_lock_op(sbi);
	err = f2fs_add_link(dentry, inode);
	mutex_unlock_op(sbi, ilock);
	if (err)
		goto out;

	alloc_nid_done(sbi, ino);

	if (!sbi->por_doing)
		d_instantiate(dentry, inode);
	unlock_new_inode(inode);
	return 0;
out:
	clear_nlink(inode);
	unlock_new_inode(inode);
	make_bad_inode(inode);
	iput(inode);
	alloc_nid_failed(sbi, ino);
	return err;
}

static int f2fs_link(struct dentry *old_dentry, struct inode *dir,
		struct dentry *dentry)
{
	struct inode *inode = old_dentry->d_inode;
	struct super_block *sb = dir->i_sb;
	struct f2fs_sb_info *sbi = F2FS_SB(sb);
	int err, ilock;

	f2fs_balance_fs(sbi);

	inode->i_ctime = CURRENT_TIME;
	atomic_inc(&inode->i_count);

	set_inode_flag(F2FS_I(inode), FI_INC_LINK);
	ilock = mutex_lock_op(sbi);
	err = f2fs_add_link(dentry, inode);
	mutex_unlock_op(sbi, ilock);
	if (err)
		goto out;

	/*
	 * This file should be checkpointed during fsync.
	 * We lost i_pino from now on.
	 */
	set_cp_file(inode);

	d_instantiate(dentry, inode);
	return 0;
out:
	clear_inode_flag(F2FS_I(inode), FI_INC_LINK);
	make_bad_inode(inode);
	iput(inode);
	return err;
}

struct dentry *f2fs_get_parent(struct dentry *child)
{
	struct qstr dotdot = QSTR_INIT("..", 2);
	unsigned long ino = f2fs_inode_by_name(child->d_inode, &dotdot);
	if (!ino)
		return ERR_PTR(-ENOENT);
	return d_obtain_alias(f2fs_iget(child->d_inode->i_sb, ino));
}

static struct dentry *f2fs_lookup(struct inode *dir, struct dentry *dentry,
		unsigned int flags)
{
	struct inode *inode = NULL;
	struct f2fs_dir_entry *de;
	struct page *page;

	if (dentry->d_name.len > F2FS_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	de = f2fs_find_entry(dir, &dentry->d_name, &page);
	if (de) {
		nid_t ino = le32_to_cpu(de->ino);
		kunmap(page);
		f2fs_put_page(page, 0);

		inode = f2fs_iget(dir->i_sb, ino);
		if (IS_ERR(inode))
			return ERR_CAST(inode);
	}

	return d_splice_alias(inode, dentry);
}

static int f2fs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct super_block *sb = dir->i_sb;
	struct f2fs_sb_info *sbi = F2FS_SB(sb);
	struct inode *inode = dentry->d_inode;
	struct f2fs_dir_entry *de;
	struct page *page;
	int err = -ENOENT;
	int ilock;

	trace_f2fs_unlink_enter(dir, dentry);
	f2fs_balance_fs(sbi);

	de = f2fs_find_entry(dir, &dentry->d_name, &page);
	if (!de)
		goto fail;

	err = check_orphan_space(sbi);
	if (err) {
		kunmap(page);
		f2fs_put_page(page, 0);
		goto fail;
	}

	ilock = mutex_lock_op(sbi);
	f2fs_delete_entry(de, page, inode);
	mutex_unlock_op(sbi, ilock);

	/* In order to evict this inode,  we set it dirty */
	mark_inode_dirty(inode);
fail:
	trace_f2fs_unlink_exit(inode, err);
	return err;
}

static int f2fs_symlink(struct inode *dir, struct dentry *dentry,
					const char *symname)
{
	struct super_block *sb = dir->i_sb;
	struct f2fs_sb_info *sbi = F2FS_SB(sb);
	struct inode *inode;
	size_t symlen = strlen(symname) + 1;
	int err, ilock;
	size_t len = strlen(symname);
	struct fscrypt_str disk_link = FSTR_INIT((char *)symname, len + 1);
	struct fscrypt_symlink_data *sd = NULL;
	int err;

	if (f2fs_encrypted_inode(dir)) {
		err = fscrypt_get_encryption_info(dir);
		if (err)
			return err;

		if (!fscrypt_has_encryption_key(dir))
			return -ENOKEY;

	f2fs_balance_fs(sbi);

	inode = f2fs_new_inode(dir, S_IFLNK | S_IRWXUGO);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	inode->i_op = &f2fs_symlink_inode_operations;
	inode->i_mapping->a_ops = &f2fs_dblock_aops;

	ilock = mutex_lock_op(sbi);
	err = f2fs_add_link(dentry, inode);
	mutex_unlock_op(sbi, ilock);
	if (err)
		goto out;

	err = page_symlink(inode, symname, symlen);
	alloc_nid_done(sbi, inode->i_ino);
	if (f2fs_encrypted_inode(inode)) {
		struct qstr istr = QSTR_INIT(symname, len);
		struct fscrypt_str ostr;

		sd = kzalloc(disk_link.len, GFP_NOFS);
		if (!sd) {
			err = -ENOMEM;
			goto err_out;
		}

		err = fscrypt_get_encryption_info(inode);
		if (err)
			goto err_out;

		if (!fscrypt_has_encryption_key(inode)) {
			err = -ENOKEY;
			goto err_out;
		}

		ostr.name = sd->encrypted_path;
		ostr.len = disk_link.len;
		err = fscrypt_fname_usr_to_disk(inode, &istr, &ostr);
		if (err)
			goto err_out;

		sd->len = cpu_to_le16(ostr.len);
		disk_link.name = (char *)sd;
	}

	err = page_symlink(inode, disk_link.name, disk_link.len);

	d_instantiate(dentry, inode);
	unlock_new_inode(inode);
	return err;
out:
	clear_nlink(inode);
	unlock_new_inode(inode);
	make_bad_inode(inode);
	iput(inode);
	alloc_nid_failed(sbi, inode->i_ino);
	return err;
}

static int f2fs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct f2fs_sb_info *sbi = F2FS_SB(dir->i_sb);
	struct inode *inode;
	int err, ilock;

	f2fs_balance_fs(sbi);

	inode = f2fs_new_inode(dir, S_IFDIR | mode);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	inode->i_op = &f2fs_dir_inode_operations;
	inode->i_fop = &f2fs_dir_operations;
	inode->i_mapping->a_ops = &f2fs_dblock_aops;
	mapping_set_gfp_mask(inode->i_mapping, GFP_F2FS_ZERO);

	set_inode_flag(F2FS_I(inode), FI_INC_LINK);
	ilock = mutex_lock_op(sbi);
	err = f2fs_add_link(dentry, inode);
	mutex_unlock_op(sbi, ilock);
	if (err)
		goto out_fail;

	alloc_nid_done(sbi, inode->i_ino);

	d_instantiate(dentry, inode);
	unlock_new_inode(inode);

	return 0;

out_fail:
	clear_inode_flag(F2FS_I(inode), FI_INC_LINK);
	clear_nlink(inode);
	unlock_new_inode(inode);
	make_bad_inode(inode);
	iput(inode);
	alloc_nid_failed(sbi, inode->i_ino);
	return err;
}

static int f2fs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	if (f2fs_empty_dir(inode))
		return f2fs_unlink(dir, dentry);
	return -ENOTEMPTY;
}

static int f2fs_mknod(struct inode *dir, struct dentry *dentry,
				umode_t mode, dev_t rdev)
{
	struct super_block *sb = dir->i_sb;
	struct f2fs_sb_info *sbi = F2FS_SB(sb);
	struct inode *inode;
	int err = 0;
	int ilock;

	if (!new_valid_dev(rdev))
		return -EINVAL;

	f2fs_balance_fs(sbi);

	inode = f2fs_new_inode(dir, mode);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	init_special_inode(inode, inode->i_mode, rdev);
	inode->i_op = &f2fs_special_inode_operations;

	ilock = mutex_lock_op(sbi);
	err = f2fs_add_link(dentry, inode);
	mutex_unlock_op(sbi, ilock);
	if (err)
		goto out;

	alloc_nid_done(sbi, inode->i_ino);
	d_instantiate(dentry, inode);
	unlock_new_inode(inode);
	return 0;
out:
	clear_nlink(inode);
	unlock_new_inode(inode);
	make_bad_inode(inode);
	iput(inode);
	alloc_nid_failed(sbi, inode->i_ino);
	return err;
}

static int f2fs_rename(struct inode *old_dir, struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry)
{
	struct super_block *sb = old_dir->i_sb;
	struct f2fs_sb_info *sbi = F2FS_SB(sb);
	struct inode *old_inode = old_dentry->d_inode;
	struct inode *new_inode = new_dentry->d_inode;
	struct page *old_dir_page;
	struct page *old_page;
	struct f2fs_dir_entry *old_dir_entry = NULL;
	struct f2fs_dir_entry *old_entry;
	struct f2fs_dir_entry *new_entry;
	int err = -ENOENT, ilock = -1;

	f2fs_balance_fs(sbi);

	old_entry = f2fs_find_entry(old_dir, &old_dentry->d_name, &old_page);
	if (!old_entry)
		goto out;

	if (S_ISDIR(old_inode->i_mode)) {
		err = -EIO;
		old_dir_entry = f2fs_parent_dir(old_inode, &old_dir_page);
		if (!old_dir_entry)
			goto out_old;
	}

	ilock = mutex_lock_op(sbi);

	if (new_inode) {
		struct page *new_page;

		err = -ENOTEMPTY;
		if (old_dir_entry && !f2fs_empty_dir(new_inode))
			goto out_dir;

		err = -ENOENT;
		new_entry = f2fs_find_entry(new_dir, &new_dentry->d_name,
						&new_page);
		if (!new_entry)
			goto out_dir;

		f2fs_set_link(new_dir, new_entry, new_page, old_inode);

		new_inode->i_ctime = CURRENT_TIME;
		if (old_dir_entry)
			drop_nlink(new_inode);
		drop_nlink(new_inode);
		if (!new_inode->i_nlink)
			add_orphan_inode(sbi, new_inode->i_ino);
		update_inode_page(new_inode);
	} else {
		err = f2fs_add_link(new_dentry, old_inode);
		if (err)
			goto out_dir;

		if (old_dir_entry) {
			inc_nlink(new_dir);
			update_inode_page(new_dir);
		}
	}

	old_inode->i_ctime = CURRENT_TIME;
	mark_inode_dirty(old_inode);

	f2fs_delete_entry(old_entry, old_page, NULL);

	if (old_dir_entry) {
		if (old_dir != new_dir) {
			f2fs_set_link(old_inode, old_dir_entry,
						old_dir_page, new_dir);
		} else {
			kunmap(old_dir_page);
			f2fs_put_page(old_dir_page, 0);
		}
		drop_nlink(old_dir);
		update_inode_page(old_dir);
	}

	mutex_unlock_op(sbi, ilock);
	return 0;

out_dir:
	if (old_dir_entry) {
		kunmap(old_dir_page);
		f2fs_dentry_kunmap(old_inode, old_dir_page);
		f2fs_put_page(old_dir_page, 0);
	}
out_old:
	f2fs_dentry_kunmap(old_dir, old_page);
	f2fs_put_page(old_page, 0);
out:
	return err;
}

#if 0
static int f2fs_cross_rename(struct inode *old_dir, struct dentry *old_dentry,
			     struct inode *new_dir, struct dentry *new_dentry)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(old_dir);
	struct inode *old_inode = d_inode(old_dentry);
	struct inode *new_inode = d_inode(new_dentry);
	struct page *old_dir_page, *new_dir_page;
	struct page *old_page, *new_page;
	struct f2fs_dir_entry *old_dir_entry = NULL, *new_dir_entry = NULL;
	struct f2fs_dir_entry *old_entry, *new_entry;
	int old_nlink = 0, new_nlink = 0;
	int err = -ENOENT;

	if ((f2fs_encrypted_inode(old_dir) &&
			!fscrypt_has_encryption_key(old_dir)) ||
			(f2fs_encrypted_inode(new_dir) &&
			!fscrypt_has_encryption_key(new_dir)))
		return -ENOKEY;

	if ((f2fs_encrypted_inode(old_dir) || f2fs_encrypted_inode(new_dir)) &&
			(old_dir != new_dir) &&
			(!fscrypt_has_permitted_context(new_dir, old_inode) ||
			 !fscrypt_has_permitted_context(old_dir, new_inode)))
		return -EPERM;

	old_entry = f2fs_find_entry(old_dir, &old_dentry->d_name, &old_page);
	if (!old_entry) {
		if (IS_ERR(old_page))
			err = PTR_ERR(old_page);
		goto out;
	}

	new_entry = f2fs_find_entry(new_dir, &new_dentry->d_name, &new_page);
	if (!new_entry) {
		if (IS_ERR(new_page))
			err = PTR_ERR(new_page);
		goto out_old;
	}

	/* prepare for updating ".." directory entry info later */
	if (old_dir != new_dir) {
		if (S_ISDIR(old_inode->i_mode)) {
			old_dir_entry = f2fs_parent_dir(old_inode,
							&old_dir_page);
			if (!old_dir_entry) {
				if (IS_ERR(old_dir_page))
					err = PTR_ERR(old_dir_page);
				goto out_new;
			}
		}

		if (S_ISDIR(new_inode->i_mode)) {
			new_dir_entry = f2fs_parent_dir(new_inode,
							&new_dir_page);
			if (!new_dir_entry) {
				if (IS_ERR(new_dir_page))
					err = PTR_ERR(new_dir_page);
				goto out_old_dir;
			}
		}
	}

	/*
	 * If cross rename between file and directory those are not
	 * in the same directory, we will inc nlink of file's parent
	 * later, so we should check upper boundary of its nlink.
	 */
	if ((!old_dir_entry || !new_dir_entry) &&
				old_dir_entry != new_dir_entry) {
		old_nlink = old_dir_entry ? -1 : 1;
		new_nlink = -old_nlink;
		err = -EMLINK;
		if ((old_nlink > 0 && old_dir->i_nlink >= F2FS_LINK_MAX) ||
			(new_nlink > 0 && new_dir->i_nlink >= F2FS_LINK_MAX))
			goto out_new_dir;
	}

	f2fs_balance_fs(sbi, true);

	f2fs_lock_op(sbi);

	err = update_dent_inode(old_inode, new_inode, &new_dentry->d_name);
	if (err)
		goto out_unlock;
	if (file_enc_name(new_inode))
		file_set_enc_name(old_inode);

	err = update_dent_inode(new_inode, old_inode, &old_dentry->d_name);
	if (err)
		goto out_undo;
	if (file_enc_name(old_inode))
		file_set_enc_name(new_inode);

	/* update ".." directory entry info of old dentry */
	if (old_dir_entry)
		f2fs_set_link(old_inode, old_dir_entry, old_dir_page, new_dir);

	/* update ".." directory entry info of new dentry */
	if (new_dir_entry)
		f2fs_set_link(new_inode, new_dir_entry, new_dir_page, old_dir);

	/* update directory entry info of old dir inode */
	f2fs_set_link(old_dir, old_entry, old_page, new_inode);

	down_write(&F2FS_I(old_inode)->i_sem);
	file_lost_pino(old_inode);
	up_write(&F2FS_I(old_inode)->i_sem);

	old_dir->i_ctime = current_time(old_dir);
	if (old_nlink) {
		down_write(&F2FS_I(old_dir)->i_sem);
		f2fs_i_links_write(old_dir, old_nlink > 0);
		up_write(&F2FS_I(old_dir)->i_sem);
	}
	f2fs_mark_inode_dirty_sync(old_dir, false);

	/* update directory entry info of new dir inode */
	f2fs_set_link(new_dir, new_entry, new_page, old_inode);

	down_write(&F2FS_I(new_inode)->i_sem);
	file_lost_pino(new_inode);
	up_write(&F2FS_I(new_inode)->i_sem);

	new_dir->i_ctime = current_time(new_dir);
	if (new_nlink) {
		down_write(&F2FS_I(new_dir)->i_sem);
		f2fs_i_links_write(new_dir, new_nlink > 0);
		up_write(&F2FS_I(new_dir)->i_sem);
	}
	f2fs_mark_inode_dirty_sync(new_dir, false);

	f2fs_unlock_op(sbi);

	if (IS_DIRSYNC(old_dir) || IS_DIRSYNC(new_dir))
		f2fs_sync_fs(sbi->sb, 1);
	return 0;
out_undo:
	/*
	 * Still we may fail to recover name info of f2fs_inode here
	 * Drop it, once its name is set as encrypted
	 */
	update_dent_inode(old_inode, old_inode, &old_dentry->d_name);
out_unlock:
	f2fs_unlock_op(sbi);
out_new_dir:
	if (new_dir_entry) {
		f2fs_dentry_kunmap(new_inode, new_dir_page);
		f2fs_put_page(new_dir_page, 0);
	}
out_old_dir:
	if (old_dir_entry) {
		f2fs_dentry_kunmap(old_inode, old_dir_page);
		f2fs_put_page(old_dir_page, 0);
	}
	mutex_unlock_op(sbi, ilock);
out_old:
	kunmap(old_page);
	f2fs_put_page(old_page, 0);
out:
	return err;
}

const struct inode_operations f2fs_dir_inode_operations = {
	.create		= f2fs_create,
	.lookup		= f2fs_lookup,
	.link		= f2fs_link,
	.unlink		= f2fs_unlink,
	.symlink	= f2fs_symlink,
	.mkdir		= f2fs_mkdir,
	.rmdir		= f2fs_rmdir,
	.mknod		= f2fs_mknod,
	.rename		= f2fs_rename,
	.setattr	= f2fs_setattr,
	.get_acl	= f2fs_get_acl,
#ifdef CONFIG_F2FS_FS_XATTR
	.setxattr	= generic_setxattr,
	.getxattr	= generic_getxattr,
	.listxattr	= f2fs_listxattr,
	.removexattr	= generic_removexattr,
#endif
};

const struct inode_operations f2fs_symlink_inode_operations = {
	.readlink       = generic_readlink,
	.follow_link    = page_follow_link_light,
	.put_link       = page_put_link,
	.setattr	= f2fs_setattr,
#ifdef CONFIG_F2FS_FS_XATTR
	.setxattr	= generic_setxattr,
	.getxattr	= generic_getxattr,
	.listxattr	= f2fs_listxattr,
	.removexattr	= generic_removexattr,
#endif
};

const struct inode_operations f2fs_special_inode_operations = {
	.setattr        = f2fs_setattr,
	.get_acl	= f2fs_get_acl,
#ifdef CONFIG_F2FS_FS_XATTR
	.setxattr       = generic_setxattr,
	.getxattr       = generic_getxattr,
	.listxattr	= f2fs_listxattr,
	.removexattr    = generic_removexattr,
#endif
};
