/*****************************************************************************

Copyright (c) 1997, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2020, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file include/log0recv.h
Recovery

Created 9/20/1997 Heikki Tuuri
*******************************************************/

#ifndef log0recv_h
#define log0recv_h

#include "ut0byte.h"
#include "buf0types.h"
#include "log0log.h"
#include "mtr0types.h"

#include <deque>

/** Is recv_writer_thread active? */
extern bool	recv_writer_thread_active;

/** @return whether recovery is currently running. */
#define recv_recovery_is_on() UNIV_UNLIKELY(recv_recovery_on)

/** Find the latest checkpoint in the log header.
@param[out]	max_field	LOG_CHECKPOINT_1 or LOG_CHECKPOINT_2
@return error code or DB_SUCCESS */
dberr_t
recv_find_max_checkpoint(ulint* max_field)
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Remove records for a corrupted page.
This function should called when srv_force_recovery > 0.
@param[in]	page_id page id of the corrupted page */
ATTRIBUTE_COLD void recv_recover_corrupt_page(page_id_t page_id);

/** Apply any buffered redo log to a page that was just read from a data file.
@param[in,out]	space	tablespace
@param[in,out]	bpage	buffer pool page */
ATTRIBUTE_COLD void recv_recover_page(fil_space_t* space, buf_page_t* bpage)
	MY_ATTRIBUTE((nonnull));

/** Start recovering from a redo log checkpoint.
@see recv_recovery_from_checkpoint_finish
@param[in]	flush_lsn	FIL_PAGE_FILE_FLUSH_LSN
of first system tablespace page
@return error code or DB_SUCCESS */
dberr_t
recv_recovery_from_checkpoint_start(
	lsn_t	flush_lsn);
/** Complete recovery from a checkpoint. */
void
recv_recovery_from_checkpoint_finish(void);
/********************************************************//**
Initiates the rollback of active transactions. */
void
recv_recovery_rollback_active(void);
/*===============================*/

/********************************************************//**
Reset the state of the recovery system variables. */
void
recv_sys_var_init(void);
/*===================*/

/** Apply recv_sys.pages to persistent data pages.
@param[in]	last_batch	whether redo log writes are possible */
void recv_apply_hashed_log_recs(bool last_batch);

/** Whether to store redo log records in recv_sys.pages */
enum store_t {
	/** Do not store redo log records. */
	STORE_NO,
	/** Store redo log records. */
	STORE_YES,
	/** Store redo log records if the tablespace exists. */
	STORE_IF_EXISTS
};


/** Adds data from a new log block to the parsing buffer of recv_sys if
recv_sys.parse_start_lsn is non-zero.
@param[in]	log_block	log block to add
@param[in]	scanned_lsn	lsn of how far we were able to find
				data in this log block
@return true if more data added */
bool recv_sys_add_to_parsing_buf(const byte* log_block, lsn_t scanned_lsn);

/** Parse log records from a buffer and optionally store them to recv_sys.pages
to wait merging to file pages.
@param[in]	checkpoint_lsn		the LSN of the latest checkpoint
@param[in]	store			whether to store page operations
@param[in]	available_memory	memory to read the redo logs
@param[in]	apply			whether to apply the records
@return whether MLOG_CHECKPOINT record was seen the first time,
or corruption was noticed */
bool recv_parse_log_recs(
	lsn_t		checkpoint_lsn,
	store_t*	store,
	ulint		available_memory,
	bool		apply);

/** Moves the parsing buffer data left to the buffer start */
void recv_sys_justify_left_parsing_buf();

/** Report an operation to create, delete, or rename a file during backup.
@param[in]	space_id	tablespace identifier
@param[in]	create		whether the file is being created
@param[in]	name		file name (not NUL-terminated)
@param[in]	len		length of name, in bytes
@param[in]	new_name	new file name (NULL if not rename)
@param[in]	new_len		length of new_name, in bytes (0 if NULL) */
extern void (*log_file_op)(ulint space_id, bool create,
			   const byte* name, ulint len,
			   const byte* new_name, ulint new_len);

/** Stored redo log record */
struct log_rec_t
{
  log_rec_t(lsn_t lsn) : next(nullptr), lsn(lsn) { ut_ad(lsn); }
  log_rec_t()= delete;
  log_rec_t(const log_rec_t&)= delete;
  log_rec_t &operator=(const log_rec_t&)= delete;

  /** next record */
  log_rec_t *next;
  /** mtr_t::commit_lsn() of the mini-transaction */
  const lsn_t lsn;
};

struct recv_dblwr_t {
	/** Add a page frame to the doublewrite recovery buffer. */
	void add(byte* page) {
		pages.push_front(page);
	}

	/** Find a doublewrite copy of a page.
	@param[in]	space_id	tablespace identifier
	@param[in]	page_no		page number
	@return	page frame
	@retval NULL if no page was found */
	const byte* find_page(ulint space_id, ulint page_no);

	typedef std::deque<byte*, ut_allocator<byte*> > list;

	/** Recovered doublewrite buffer page frames */
	list	pages;
};

/** the recovery state and buffered records for a page */
struct page_recv_t
{
  /** Recovery state */
  enum
  {
    /** not yet processed */
    RECV_NOT_PROCESSED,
    /** not processed; the page will be reinitialized */
    RECV_WILL_NOT_READ,
    /** page is being read */
    RECV_BEING_READ,
    /** log records are being applied on the page */
    RECV_BEING_PROCESSED
  } state= RECV_NOT_PROCESSED;
  /** log records for a page */
  class recs_t
  {
    /** The first log record */
    log_rec_t *head= NULL;
    /** The last log record */
    log_rec_t *tail= NULL;
  public:
    /** Append a redo log snippet for the page
    @param recs log snippet */
    void append(log_rec_t* recs)
    {
      if (tail)
        tail->next= recs;
      else
        head= recs;
      tail= recs;
    }

    /** Trim old log records for a page
    @param start_lsn oldest log sequence number to preserve
    @return whether the entire log was trimmed */
    inline bool trim(lsn_t start_lsn);
    /** @return the last log snippet */
    const log_rec_t* last() const { return tail; }

    class iterator
    {
      log_rec_t *cur;
    public:
      iterator(log_rec_t* rec) : cur(rec) {}
      log_rec_t* operator*() const { return cur; }
      iterator &operator++() { cur= cur->next; return *this; }
      bool operator!=(const iterator& i) const { return cur != i.cur; }
    };
    iterator begin() { return head; }
    iterator end() { return NULL; }
    bool empty() const { ut_ad(!head == !tail); return !head; }
    inline void clear();
  } log;

  /** Ignore any earlier redo log records for this page. */
  inline void will_not_read();
  /** @return whether the log records for the page are being processed */
  bool is_being_processed() const { return state == RECV_BEING_PROCESSED; }
};

/** Recovery system data structure */
struct recv_sys_t{
	ib_mutex_t		mutex;	/*!< mutex protecting the fields apply_log_recs,
				n_addrs, and the state field in each recv_addr
				struct */
	ib_mutex_t		writer_mutex;/*!< mutex coordinating
				flushing between recv_writer_thread and
				the recovery thread. */
	os_event_t		flush_start;/*!< event to activate
				page cleaner threads */
	os_event_t		flush_end;/*!< event to signal that the page
				cleaner has finished the request */
	buf_flush_t		flush_type;/*!< type of the flush request.
				BUF_FLUSH_LRU: flush end of LRU, keeping free blocks.
				BUF_FLUSH_LIST: flush all of blocks. */
	/** whether recv_recover_page(), invoked from buf_page_io_complete(),
	should apply log records*/
	bool		apply_log_recs;
	/** whether recv_apply_hashed_log_recs() is running */
	bool		apply_batch_on;
	byte*		buf;	/*!< buffer for parsing log records */
	size_t		buf_size;	/*!< size of buf */
	ulint		len;	/*!< amount of data in buf */
	lsn_t		parse_start_lsn;
				/*!< this is the lsn from which we were able to
				start parsing log records and adding them to
				pages; zero if a suitable
				start point not found yet */
	lsn_t		scanned_lsn;
				/*!< the log data has been scanned up to this
				lsn */
	ulint		scanned_checkpoint_no;
				/*!< the log data has been scanned up to this
				checkpoint number (lowest 4 bytes) */
	ulint		recovered_offset;
				/*!< start offset of non-parsed log records in
				buf */
	lsn_t		recovered_lsn;
				/*!< the log records have been parsed up to
				this lsn */
	bool		found_corrupt_log;
				/*!< set when finding a corrupt log
				block or record, or there is a log
				parsing buffer overflow */
	bool		found_corrupt_fs;
				/*!< set when an inconsistency with
				the file system contents is detected
				during log scan or apply */
	lsn_t		mlog_checkpoint_lsn;
				/*!< the LSN of a MLOG_CHECKPOINT
				record, or 0 if none was parsed */
	/** the time when progress was last reported */
	time_t		progress_time;
	mem_heap_t*	heap;	/*!< memory heap of log records and file
				addresses*/

	using map = std::map<const page_id_t, page_recv_t,
			     std::less<const page_id_t>,
			     ut_allocator
			     <std::pair<const page_id_t, page_recv_t>>>;
	/** buffered records waiting to be applied to pages */
	map pages;

	/** Process a record that indicates that a tablespace is
	being shrunk in size.
	@param page_id	first page identifier that is not in the file
	@param lsn	log sequence number of the shrink operation */
	inline void trim(const page_id_t page_id, lsn_t lsn);

	/** Undo tablespaces for which truncate has been logged
	(indexed by id - srv_undo_space_id_start) */
	struct trunc {
		/** log sequence number of MLOG_FILE_CREATE2, or 0 if none */
		lsn_t		lsn;
		/** truncated size of the tablespace, or 0 if not truncated */
		unsigned	pages;
	} truncated_undo_spaces[127];

	recv_dblwr_t	dblwr;

	/** Last added LSN to pages. */
	lsn_t		last_stored_lsn;

	/** Initialize the redo log recovery subsystem. */
	void create();

	/** Free most recovery data structures. */
	void debug_free();

	/** Clean up after create() */
	void close();

	bool is_initialised() const { return buf_size != 0; }

	/** Store a redo log record for applying.
	@param type	record type
	@param page_id	page identifier
	@param body	record body
	@param rec_end	end of record
	@param lsn	start LSN of the mini-transaction
	@param end_lsn	end LSN of the mini-transaction */
	inline void add(mlog_id_t type, const page_id_t page_id,
			const byte* body, const byte* rec_end, lsn_t lsn,
			lsn_t end_lsn);

#if 1 /* MDEV-21351 FIXME: remove this */
	/** Register a redo log snippet for a page.
	@param page_id  page identifier
	@param start_lsn start LSN of the mini-transaction
	@param lsn      @see mtr_t::commit_lsn()
	@param l        redo log snippet @see log_t::FORMAT_10_5
	@param len      length of l, in bytes */
	inline void add(const page_id_t page_id, lsn_t start_lsn, lsn_t lsn,
			const byte* l, size_t len);
#else
	/** Register a redo log snippet for a page.
	@param page_id  page identifier
	@param start_lsn start LSN of the mini-transaction
	@param lsn      @see mtr_t::commit_lsn()
	@param l        redo log snippet @see log_t::FORMAT_10_5 */
	inline void add(const page_id_t page_id, lsn_t start_lsn, lsn_t lsn,
			const byte* l);
#endif

	/**
	Parse and register one mini-transaction in log_t::FORMAT_10_5.
	@param checkpoint_lsn  the log sequence number of the latest checkpoint
	@param store           whether to store the records
	@param apply           whether to apply file-level log records
	@return whether MLOG_CHECKPOINT record was seen the first time,
	or corruption was noticed */
        inline bool parse(lsn_t checkpoint_lsn, store_t store, bool apply);

	/** Clear a fully processed set of stored redo log records. */
	inline void clear();

	/** Determine whether redo log recovery progress should be reported.
	@param[in]	time	the current time
	@return	whether progress should be reported
		(the last report was at least 15 seconds ago) */
	bool report(time_t time)
	{
		if (time - progress_time < 15) {
			return false;
		}

		progress_time = time;
		return true;
	}
};

/** The recovery system */
extern recv_sys_t	recv_sys;

/** TRUE when applying redo log records during crash recovery; FALSE
otherwise.  Note that this is FALSE while a background thread is
rolling back incomplete transactions. */
extern volatile bool	recv_recovery_on;
/** If the following is TRUE, the buffer pool file pages must be invalidated
after recovery and no ibuf operations are allowed; this will be set if
recv_sys.pages becomes too full, and log records must be merged
to file pages already before the recovery is finished: in this case no
ibuf operations are allowed, as they could modify the pages read in the
buffer pool before the pages have been recovered to the up-to-date state.

TRUE means that recovery is running and no operations on the log files
are allowed yet: the variable name is misleading. */
extern bool		recv_no_ibuf_operations;
/** TRUE when recv_init_crash_recovery() has been called. */
extern bool		recv_needed_recovery;
#ifdef UNIV_DEBUG
/** TRUE if writing to the redo log (mtr_commit) is forbidden.
Protected by log_sys.mutex. */
extern bool		recv_no_log_write;
#endif /* UNIV_DEBUG */

/** TRUE if buf_page_is_corrupted() should check if the log sequence
number (FIL_PAGE_LSN) is in the future.  Initially FALSE, and set by
recv_recovery_from_checkpoint_start(). */
extern bool		recv_lsn_checks_on;

/** Size of the parsing buffer; it must accommodate RECV_SCAN_SIZE many
times! */
#define RECV_PARSING_BUF_SIZE	(2U << 20)

/** Size of block reads when the log groups are scanned forward to do a
roll-forward */
#define RECV_SCAN_SIZE		(4U << srv_page_size_shift)

/** This many frames must be left free in the buffer pool when we scan
the log and store the scanned log records in the buffer pool: we will
use these free frames to read in pages when we start applying the
log records to the database. */
extern ulint	recv_n_pool_free_frames;

#endif
