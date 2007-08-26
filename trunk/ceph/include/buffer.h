// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software 
 * Foundation.  See file COPYING.
 * 
 */

#ifndef __BUFFER_H
#define __BUFFER_H

#include <stdint.h>

#include "common/Mutex.h"


//#define BUFFER_USE_CCPP

#ifdef BUFFER_USE_CCPP
# include "cc++/thread.h"
#endif

#include <iostream>
#include <list>

using std::cout;

#ifndef __CYGWIN__
# include <sys/mman.h>
#endif

#define BUFFER_PAGE_SIZE 4096  // FIXME

// <hack>
//  these are in config.o
extern Mutex bufferlock;
extern long buffer_total_alloc;
// </hack>




class buffer {
private:
  
  /* hack for memory utilization debugging. */
  static void inc_total_alloc(unsigned len) {
    bufferlock.Lock();
    buffer_total_alloc += len;
    bufferlock.Unlock();
  }
  static void dec_total_alloc(unsigned len) {
    bufferlock.Lock();
    buffer_total_alloc -= len;
    bufferlock.Unlock();
  }

  /*
   * an abstract raw buffer.  with a reference count.
   */
  class raw {
  public:
    char *data;
    unsigned len;
#ifdef BUFFER_USE_CCPP
    mutable ost::AtomicCounter nref;    // mutable for const-ness of operator<<
#else
    int nref;
    Mutex lock;  // we'll make it non-recursive.
#endif

    raw(unsigned l) : len(l), nref(0)
#ifndef BUFFER_USE_CCPP
		      , lock(false)
#endif
    { }
    raw(char *c, unsigned l) : data(c), len(l), nref(0)
#ifndef BUFFER_USE_CCPP
			       , lock(false)
#endif
    { }
    virtual ~raw() {};

    // no copying.
    raw(const raw &other);
    const raw& operator=(const raw &other);

    virtual raw* clone_empty() = 0;
    raw *clone() {
      raw *c = clone_empty();
      memcpy(c->data, data, len);
      return c;
    }

    bool is_page_aligned() {
      return (long)data % BUFFER_PAGE_SIZE == 0;
    }
  };

  friend std::ostream& operator<<(std::ostream& out, const raw &r);

  /*
   * primitive buffer types
   */
  class raw_char : public raw {
  public:
    raw_char(unsigned l) : raw(l) {
      data = new char[len];
      inc_total_alloc(len);
    }
    ~raw_char() {
      delete[] data;
      dec_total_alloc(len);      
    }
    raw* clone_empty() {
      return new raw_char(len);
    }
  };

  class raw_static : public raw {
  public:
    raw_static(const char *d, unsigned l) : raw((char*)d, l) { }
    ~raw_static() {}
    raw* clone_empty() {
      return new raw_char(len);
    }
  };

#ifndef __CYGWIN__
  class raw_mmap_pages : public raw {
  public:
    raw_mmap_pages(unsigned l) : raw(l) {
      data = (char*)::mmap(NULL, len, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
      inc_total_alloc(len);
    }
    ~raw_mmap_pages() {
      ::munmap(data, len);
      dec_total_alloc(len);
    }
    raw* clone_empty() {
      return new raw_mmap_pages(len);
    }
  };

  class raw_posix_aligned : public raw {
  public:
    raw_posix_aligned(unsigned l) : raw(l) {
#ifdef DARWIN
      data = (char *) valloc (len);
#else
      ::posix_memalign((void**)(void*)&data, BUFFER_PAGE_SIZE, len);
#endif /* DARWIN */
      inc_total_alloc(len);
    }
    ~raw_posix_aligned() {
      ::free((void*)data);
      dec_total_alloc(len);
    }
    raw* clone_empty() {
      return new raw_posix_aligned(len);
    }
  };
#endif

#ifdef __CYGWIN__
  class raw_hack_aligned : public raw {
    char *realdata;
  public:
    raw_hack_aligned(unsigned l) : raw(l) {
      realdata = new char[len+BUFFER_PAGE_SIZE-1];
      unsigned off = ((unsigned)realdata) % BUFFER_PAGE_SIZE;
      if (off) 
	data = realdata + BUFFER_PAGE_SIZE - off;
      else
	data = realdata;
      inc_total_alloc(len+BUFFER_PAGE_SIZE-1);
      //cout << "hack aligned " << (unsigned)data 
      //<< " in raw " << (unsigned)realdata
      //<< " off " << off << std::endl;
      assert(((unsigned)data & (BUFFER_PAGE_SIZE-1)) == 0);
    }
    ~raw_hack_aligned() {
      delete[] realdata;
      dec_total_alloc(len+BUFFER_PAGE_SIZE-1);
    }
    raw* clone_empty() {
      return new raw_hack_aligned(len);
    }
  };
#endif

public:

  /*
   * named constructors 
   */

  static raw* copy(const char *c, unsigned len) {
    raw* r = new raw_char(len);
    memcpy(r->data, c, len);
    return r;
  }
  static raw* create(unsigned len) {
    return new raw_char(len);
  }

  static raw* create_page_aligned(unsigned len) {
#ifndef __CYGWIN__
    //return new raw_mmap_pages(len);
    return new raw_posix_aligned(len);
#else
    return new raw_hack_aligned(len);
#endif
  }
  
  
  /*
   * a buffer pointer.  references (a subsequence of) a raw buffer.
   */
  class ptr {
    raw *_raw;
    unsigned _off, _len;

  public:
    ptr() : _raw(0), _off(0), _len(0) {}
    ptr(raw *r) : _raw(r), _off(0), _len(r->len) {   // no lock needed; this is an unref raw.
      ++r->nref;
    }
    ptr(unsigned l) : _off(0), _len(l) {
      _raw = create(l);
      ++_raw->nref;
    }
    ptr(char *d, unsigned l) : _off(0), _len(l) {    // ditto.
      _raw = copy(d, l);
      ++_raw->nref;
    }
    ptr(const ptr& p) : _raw(p._raw), _off(p._off), _len(p._len) {
      if (_raw) {
#ifdef BUFFER_USE_CCPP
	++_raw->nref;
#else
	_raw->lock.Lock();
        ++_raw->nref;
       _raw->lock.Unlock();
#endif
      }
    }
    ptr(const ptr& p, unsigned o, unsigned l) : _raw(p._raw), _off(p._off + o), _len(l) {
      assert(o+l <= p._len);
      assert(_raw);
#ifdef BUFFER_USE_CCPP
      ++_raw->nref;
#else
      _raw->lock.Lock();
      ++_raw->nref;
      _raw->lock.Unlock();
#endif
    }
    ptr& operator= (const ptr& p) {
      // be careful -- we need to properly handle self-assignment.
      if (p._raw) {
#ifdef BUFFER_USE_CCPP
	++p._raw->nref;                              // inc new
#else
	p._raw->lock.Lock();
	++p._raw->nref;                              // inc new
	p._raw->lock.Unlock();
#endif
      }
      release();                                 // dec (+ dealloc) old (if any)
      _raw = p._raw;                               // change my ref
      _off = p._off;
      _len = p._len;
      return *this;
    }
    ~ptr() {
      release();
    }

    void swap(ptr& other) {
      raw *r = _raw;
      unsigned o = _off;
      unsigned l = _len;
      _raw = other._raw;
      _off = other._off;
      _len = other._len;
      other._raw = r;
      other._off = o;
      other._len = l;
    }

    void release() {
      if (_raw) {
#ifndef BUFFER_USE_CCPP
	_raw->lock.Lock();
#endif
        if (--_raw->nref == 0) {
          //cout << "hosing raw " << (void*)_raw << " len " << _raw->len << std::endl;
#ifndef BUFFER_USE_CCPP
	  _raw->lock.Unlock();    
#endif
          delete _raw;  // dealloc old (if any)
	} else {
#ifndef BUFFER_USE_CCPP
	  _raw->lock.Unlock();    
#endif
	}
	_raw = 0;
      }
    }

    // misc
    bool at_buffer_head() const { return _off == 0; }
    bool at_buffer_tail() const { return _off + _len == _raw->len; }

    bool is_page_aligned() const { return (long)c_str() % BUFFER_PAGE_SIZE == 0; }

    // accessors
    const char *c_str() const { assert(_raw); return _raw->data + _off; }
    char *c_str() { assert(_raw); return _raw->data + _off; }
    unsigned length() const { return _len; }
    unsigned offset() const { return _off; }
    unsigned start() const { return _off; }
    unsigned end() const { return _off + _len; }
    unsigned unused_tail_length() const { 
      if (_raw)
	return _raw->len - (_off+_len); 
      else
	return 0;
    }
    const char& operator[](unsigned n) const { 
      assert(_raw); 
      assert(n < _len);
      return _raw->data[_off + n];
    }
    char& operator[](unsigned n) { 
      assert(_raw); 
      assert(n < _len);
      return _raw->data[_off + n];
    }

    const char *raw_c_str() const { assert(_raw); return _raw->data; }
    unsigned raw_length() const { assert(_raw); return _raw->len; }
    int raw_nref() const { assert(_raw); return _raw->nref; }

    void copy_out(unsigned o, unsigned l, char *dest) const {
      assert(_raw);
      assert(o >= 0 && o <= _len);
      assert(l >= 0 && o+l <= _len);
      memcpy(dest, c_str()+o, l);
    }

    unsigned wasted() {
      assert(_raw);
      return _raw->len - _len;
    }

    // modifiers
    void set_offset(unsigned o) { _off = o; }
    void set_length(unsigned l) { _len = l; }

    void append(const char *p, unsigned l) {
      assert(_raw);
      assert(l <= unused_tail_length());
      memcpy(c_str() + _len, p, l);
      _len += l;
    }

    void copy_in(unsigned o, unsigned l, const char *src) {
      assert(_raw);
      assert(o >= 0 && o <= _len);
      assert(l >= 0 && o+l <= _len);
      memcpy(c_str()+o, src, l);
    }

    void zero() {
      memset(c_str(), 0, _len);
    }

    void clean() {
      //raw *newraw = _raw->makesib(_len);
    }
  };

  friend std::ostream& operator<<(std::ostream& out, const buffer::ptr& bp);

  /*
   * list - the useful bit!
   */

  class list {
    // my private bits
    std::list<ptr> _buffers;
    unsigned _len;
    ptr append_buffer;  // where i put small appends.

  public:
    // cons/des
    list() : _len(0) {}
    list(const list& other) : _buffers(other._buffers), _len(other._len) { }
    list(unsigned l) : _len(0) {
      ptr bp(l);
      push_back(bp);
    }
    ~list() {}
    
    list& operator= (const list& other) {
      _buffers = other._buffers;
      _len = other._len;
      return *this;
    }

    const std::list<ptr>& buffers() const { return _buffers; }
    
    void swap(list& other) {
      unsigned t = _len;
      _len = other._len;
      other._len = t;
      _buffers.swap(other._buffers);
      append_buffer.swap(other.append_buffer);
    }

    unsigned length() const {
#if 0
      // DEBUG: verify _len
      unsigned len = 0;
      for (std::list<ptr>::const_iterator it = _buffers.begin();
	   it != _buffers.end();
	   it++) {
	len += (*it).length();
      }
      assert(len == _len);
#endif
      return _len;
    }

    bool is_page_aligned() const {
      for (std::list<ptr>::const_iterator it = _buffers.begin();
	   it != _buffers.end();
	   it++) 
	if (!it->is_page_aligned()) return false;
      return true;
    }
    bool is_n_page_sized() const {
      return length() % BUFFER_PAGE_SIZE == 0;
    }

    // modifiers
    void clear() {
      _buffers.clear();
      _len = 0;
    }
    void push_front(ptr& bp) {
      _buffers.push_front(bp);
      _len += bp.length();
    }
    void push_front(raw *r) {
      ptr bp(r);
      _buffers.push_front(bp);
      _len += bp.length();
    }
    void push_back(ptr& bp) {
      _buffers.push_back(bp);
      _len += bp.length();
    }
    void push_back(raw *r) {
      ptr bp(r);
      _buffers.push_back(bp);
      _len += bp.length();
    }
    void zero() {
      for (std::list<ptr>::iterator it = _buffers.begin();
	   it != _buffers.end();
	   it++)
        it->zero();
    }

    // sort-of-like-assignment-op
    void claim(list& bl) {
      // free my buffers
      clear();
      claim_append(bl);
    }
    void claim_append(list& bl) {
      // steal the other guy's buffers
      _len += bl._len;
      _buffers.splice( _buffers.end(), bl._buffers );
      bl._len = 0;
    }
    
    // crope lookalikes
    void copy(unsigned off, unsigned len, char *dest) {
      assert(off >= 0);
      assert(off + len <= length());
      /*assert(off < length());
	if (off + len > length()) 
	len = length() - off;
      */
      // advance to off
      std::list<ptr>::iterator curbuf = _buffers.begin();
      
      // skip off
      while (off > 0) {
	assert(curbuf != _buffers.end());
	if (off >= (*curbuf).length()) {
	  // skip this buffer
	  off -= (*curbuf).length();
	  curbuf++;
	} else {
	  // somewhere in this buffer!
	  break;
	}
      }
      
      // copy
      while (len > 0) {
	// is the rest ALL in this buffer?
	if (off + len <= (*curbuf).length()) {
	  (*curbuf).copy_out(off, len, dest);        // yup, last bit!
	  break;
	}
	
	// get as much as we can from this buffer.
	unsigned howmuch = (*curbuf).length() - off;
	(*curbuf).copy_out(off, howmuch, dest);
	
	dest += howmuch;
	len -= howmuch;
	off = 0;
	curbuf++;
	assert(curbuf != _buffers.end());
      }
    }
    
    void copy_in(unsigned off, unsigned len, const char *src) {
      assert(off >= 0);
      assert(off + len <= length());
      
      // advance to off
      std::list<ptr>::iterator curbuf = _buffers.begin();
      
      // skip off
      while (off > 0) {
	assert(curbuf != _buffers.end());
	if (off >= (*curbuf).length()) {
	  // skip this buffer
	  off -= (*curbuf).length();
	  curbuf++;
	} else {
	  // somewhere in this buffer!
	  break;
	}
      }
      
      // copy
      while (len > 0) {
	// is the rest ALL in this buffer?
	if (off + len <= (*curbuf).length()) {
	  (*curbuf).copy_in(off, len, src);        // yup, last bit!
	  break;
	}
	
	// get as much as we can from this buffer.
	unsigned howmuch = (*curbuf).length() - off;
	(*curbuf).copy_in(off, howmuch, src);
	
	src += howmuch;
	len -= howmuch;
	off = 0;
	curbuf++;
	assert(curbuf != _buffers.end());
      }
    }
    void copy_in(unsigned off, unsigned len, const list& bl) {
      unsigned left = len;
      for (std::list<ptr>::const_iterator i = bl._buffers.begin();
	   i != bl._buffers.end();
	   i++) {
	unsigned l = (*i).length();
	if (left < l) l = left;
	copy_in(off, l, (*i).c_str());
	left -= l;
	if (left == 0) break;
	off += l;
      }
    }


    void append(const char *data, unsigned len) {
      while (len > 0) {
	// put what we can into the existing append_buffer.
	unsigned gap = append_buffer.unused_tail_length();
	if (gap > 0) {
	  if (gap > len) gap = len;
	  append_buffer.append(data, gap);
	  append(append_buffer, append_buffer.end() - gap, gap);	// add segment to the list
	  len -= gap;
	  data += gap;
	}
	if (len == 0) break;  // done!
	
	// make a new append_buffer!
	unsigned alen = BUFFER_PAGE_SIZE * (((len-1) / BUFFER_PAGE_SIZE) + 1);
	append_buffer = create_page_aligned(alen);
	append_buffer.set_length(0);   // unused, so far.
      }
    }
    void append(ptr& bp) {
      push_back(bp);
    }
    void append(ptr& bp, unsigned off, unsigned len) {
      assert(len+off <= bp.length());
      ptr tempbp(bp, off, len);
      push_back(tempbp);
    }
    void append(const list& bl) {
      _len += bl._len;
      for (std::list<ptr>::const_iterator p = bl._buffers.begin();
	   p != bl._buffers.end();
	   ++p) 
	_buffers.push_back(*p);
    }
    
    
    /*
     * get a char
     */
    const char& operator[](unsigned n) {
      assert(n < _len);
      for (std::list<ptr>::iterator p = _buffers.begin();
	   p != _buffers.end();
	   p++) {
	if (n >= p->length()) {
	  n -= p->length();
	  continue;
	}
	return (*p)[n];
      }
      assert(0);
    }

    /*
     * return a contiguous ptr to whole bufferlist contents.
     */
    char *c_str() {
      if (_buffers.size() == 1) {
	return _buffers.front().c_str();  // good, we're already contiguous.
      }
      else if (_buffers.size() == 0) {
	return 0;                         // no buffers
      } 
      else {
	ptr newbuf = create(length());	     // make one new contiguous buffer.
	copy(0, length(), newbuf.c_str());   // copy myself into it.
	clear();
	push_back(newbuf);
	return newbuf.c_str();	// now it'll work.
      }
    }

    void substr_of(const list& other, unsigned off, unsigned len) {
      assert(off + len <= other.length());
      clear();
      
      // skip off
      std::list<ptr>::const_iterator curbuf = other._buffers.begin();
      while (off > 0) {
	assert(curbuf != _buffers.end());
	if (off >= (*curbuf).length()) {
	  // skip this buffer
	  //cout << "skipping over " << *curbuf << std::endl;
	  off -= (*curbuf).length();
	  curbuf++;
	} else {
	  // somewhere in this buffer!
	  //cout << "somewhere in " << *curbuf << std::endl;
	  break;
	}
      }
      
      while (len > 0) {
	// partial?
	if (off + len < (*curbuf).length()) {
	  //cout << "copying partial of " << *curbuf << std::endl;
	  _buffers.push_back( ptr( *curbuf, off, len ) );
	  _len += len;
	  break;
	}
	
	// through end
	//cout << "copying end (all?) of " << *curbuf << std::endl;
	unsigned howmuch = (*curbuf).length() - off;
	_buffers.push_back( ptr( *curbuf, off, howmuch ) );
	_len += howmuch;
	len -= howmuch;
	off = 0;
	curbuf++;
      }
    }


    // funky modifer
    void splice(unsigned off, unsigned len, list *claim_by=0 /*, bufferlist& replace_with */) {    // fixme?
      assert(off < length()); 
      assert(len > 0);
      //cout << "splice off " << off << " len " << len << " ... mylen = " << length() << std::endl;
      
      // skip off
      std::list<ptr>::iterator curbuf = _buffers.begin();
      while (off > 0) {
	assert(curbuf != _buffers.end());
	if (off >= (*curbuf).length()) {
	  // skip this buffer
	  //cout << "off = " << off << " skipping over " << *curbuf << std::endl;
	  off -= (*curbuf).length();
	  curbuf++;
	} else {
	  // somewhere in this buffer!
	  //cout << "off = " << off << " somewhere in " << *curbuf << std::endl;
	  break;
	}
      }
      assert(off >= 0);
      
      if (off) {
	// add a reference to the front bit
	//  insert it before curbuf (which we'll hose)
	//cout << "keeping front " << off << " of " << *curbuf << std::endl;
	_buffers.insert( curbuf, ptr( *curbuf, 0, off ) );
	_len += off;
      }
      
      while (len > 0) {
	// partial?
	if (off + len < (*curbuf).length()) {
	  //cout << "keeping end of " << *curbuf << ", losing first " << off+len << std::endl;
	  if (claim_by) 
	    claim_by->append( *curbuf, off, len );
	  (*curbuf).set_offset( off+len + (*curbuf).offset() );    // ignore beginning big
	  (*curbuf).set_length( (*curbuf).length() - (len+off) );
	  _len -= off+len;
	  //cout << " now " << *curbuf << std::endl;
	  break;
	}
	
	// hose though the end
	unsigned howmuch = (*curbuf).length() - off;
	//cout << "discarding " << howmuch << " of " << *curbuf << std::endl;
	if (claim_by) 
	  claim_by->append( *curbuf, off, howmuch );
	_len -= (*curbuf).length();
	_buffers.erase( curbuf++ );
	len -= howmuch;
	off = 0;
      }
      
      // splice in *replace (implement me later?)
    }

  };

};

typedef buffer::ptr bufferptr;
typedef buffer::list bufferlist;


inline bool operator>(bufferlist& l, bufferlist& r) {
  for (unsigned p = 0; ; p++) {
    if (l.length() > p && r.length() == p) return true;
    if (l.length() == p) return false;
    if (l[p] > r[p]) return true;
    if (l[p] < r[p]) return false;
    p++;
  }
}
inline bool operator>=(bufferlist& l, bufferlist& r) {
  for (unsigned p = 0; ; p++) {
    if (l.length() > p && r.length() == p) return true;
    if (r.length() == p && l.length() == p) return true;
    if (l[p] > r[p]) return true;
    if (l[p] < r[p]) return false;
    p++;
  }
}
inline bool operator<(bufferlist& l, bufferlist& r) {
  return r > l;
}
inline bool operator<=(bufferlist& l, bufferlist& r) {
  return r >= l;
}


inline std::ostream& operator<<(std::ostream& out, const buffer::raw &r) {
  return out << "buffer::raw(" << (void*)r.data << " len " << r.len << " nref " << r.nref << ")";
}

inline std::ostream& operator<<(std::ostream& out, const buffer::ptr& bp) {
  out << "buffer::ptr(" << bp.offset() << "~" << bp.length()
      << " " << (void*)bp.c_str() 
      << " in raw " << (void*)bp.raw_c_str()
      << " len " << bp.raw_length()
      << " nref " << bp.raw_nref() << ")";
  return out;
}

inline std::ostream& operator<<(std::ostream& out, const buffer::list& bl) {
  out << "buffer::list(len=" << bl.length() << "," << std::endl;

  std::list<buffer::ptr>::const_iterator it = bl.buffers().begin();
  while (it != bl.buffers().end()) {
    out << "\t" << *it;
    if (++it == bl.buffers().end()) break;
    out << "," << std::endl;
  }
  out << std::endl << ")";
  return out;
}




// ----------------------------------------------------------
// new encoders

// raw
template<class T>
inline void _encoderaw(const T& t, bufferlist& bl)
{
  bl.append((char*)&t, sizeof(t));
}
template<class T>
inline void _decoderaw(T& t, bufferlist& bl, int& off)
{
  bl.copy(off, sizeof(t), (char*)&t);
  off += sizeof(t);
}

#include <set>
#include <map>
#include <deque>
#include <vector>
#include <string>
#include <ext/hash_map>

// list
template<class T>
inline void _encode(const std::list<T>& ls, bufferlist& bl)
{
  // should i pre- or post- count?
  if (!ls.empty()) {
    unsigned pos = bl.length();
    uint32_t n = 0;
    _encoderaw(n, bl);
    for (typename std::list<T>::const_iterator p = ls.begin(); p != ls.end(); ++p) {
      n++;
      _encode(*p, bl);
    }
    bl.copy_in(pos, sizeof(n), (char*)&n);
  } else {
    uint32_t n = ls.size();    // FIXME: this is slow on a list.
    _encoderaw(n, bl);
    for (typename std::list<T>::const_iterator p = ls.begin(); p != ls.end(); ++p)
      _encode(*p, bl);
  }
}
template<class T>
inline void _decode(std::list<T>& ls, bufferlist& bl, int& off)
{
  uint32_t n;
  _decoderaw(n, bl, off);
  ls.clear();
  while (n--) {
    T v;
    _decode(v, bl, off);
    ls.push_back(v);
  }
}

// deque
template<class T>
inline void _encode(const std::deque<T>& ls, bufferlist& bl)
{
  uint32_t n = ls.size();
  _encoderaw(n, bl);
  for (typename std::deque<T>::const_iterator p = ls.begin(); p != ls.end(); ++p)
    _encode(*p, bl);
}
template<class T>
inline void _decode(std::deque<T>& ls, bufferlist& bl, int& off)
{
  uint32_t n;
  _decoderaw(n, bl, off);
  ls.clear();
  while (n--) {
    T v;
    _decode(v, bl, off);
    ls.push_back(v);
  }
}

// set
template<class T>
inline void _encode(const std::set<T>& s, bufferlist& bl)
{
  uint32_t n = s.size();
  _encoderaw(n, bl);
  for (typename std::set<T>::const_iterator p = s.begin(); p != s.end(); ++p)
    _encode(*p, bl);
}
template<class T>
inline void _decode(std::set<T>& s, bufferlist& bl, int& off)
{
  uint32_t n;
  _decoderaw(n, bl, off);
  s.clear();
  while (n--) {
    T v;
    _decode(v, bl, off);
    s.insert(v);
  }
}

// vector
template<class T>
inline void _encode(const std::vector<T>& v, bufferlist& bl)
{
  uint32_t n = v.size();
  _encoderaw(n, bl);
  for (typename std::vector<T>::const_iterator p = v.begin(); p != v.end(); ++p)
    _encode(*p, bl);
}
template<class T>
inline void _decode(std::vector<T>& v, bufferlist& bl, int& off)
{
  uint32_t n;
  _decoderaw(n, bl, off);
  v.resize(n);
  for (uint32_t i=0; i<n; i++) 
    _decode(v[i], bl, off);
}

// map
template<class T, class U>
inline void _encode(const std::map<T,U>& m, bufferlist& bl)
{
  uint32_t n = m.size();
  _encoderaw(n, bl);
  for (typename std::map<T,U>::const_iterator p = m.begin(); p != m.end(); ++p) {
    _encode(p->first, bl);
    _encode(p->second, bl);
  }
}
template<class T, class U>
inline void _decode(std::map<T,U>& m, bufferlist& bl, int& off)
{
  uint32_t n;
  _decoderaw(n, bl, off);
  m.clear();
  while (n--) {
    T k;
    _decode(k, bl, off);
    _decode(m[k], bl, off);
  }
}

// hash_map
template<class T, class U>
inline void _encode(const __gnu_cxx::hash_map<T,U>& m, bufferlist& bl)
{
  uint32_t n = m.size();
  _encoderaw(n, bl);
  for (typename __gnu_cxx::hash_map<T,U>::const_iterator p = m.begin(); p != m.end(); ++p) {
    _encode(p->first, bl);
    _encode(p->second, bl);
  }
}
template<class T, class U>
inline void _decode(__gnu_cxx::hash_map<T,U>& m, bufferlist& bl, int& off)
{
  uint32_t n;
  _decoderaw(n, bl, off);
  m.clear();
  while (n--) {
    T k;
    _decode(k, bl, off);
    _decode(m[k], bl, off);
  }
}

// string
inline void _encode(const std::string& s, bufferlist& bl) 
{
  uint32_t len = s.length();
  _encoderaw(len, bl);
  bl.append(s.c_str(), len+1);
}
inline void _decode(std::string& s, bufferlist& bl, int& off)
{
  uint32_t len;
  _decoderaw(len, bl, off);
  s = bl.c_str() + off;    // FIXME someday to avoid a huge buffer copy?
  off += len+1;
}

// const char* (encode only, string compatible)
inline void _encode(const char *s, bufferlist& bl) 
{
  uint32_t len = strlen(s);
  _encoderaw(len, bl);
  bl.append(s, len+1);
}

// bufferptr (encapsulated)
inline void _encode(bufferptr& bp, bufferlist& bl) 
{
  uint32_t len = bp.length();
  _encoderaw(len, bl);
  bl.append(bp);
}
inline void _decode(bufferptr& bp, bufferlist& bl, int& off)
{
  uint32_t len;
  _decoderaw(len, bl, off);

  bufferlist s;
  s.substr_of(bl, off, len);
  off += len;

  if (s.buffers().size() == 1)
    bp = s.buffers().front();
  else
    bp = buffer::copy(s.c_str(), s.length());
}

// bufferlist (encapsulated)
inline void _encode(const bufferlist& s, bufferlist& bl) 
{
  uint32_t len = s.length();
  _encoderaw(len, bl);
  bl.append(s);
}
inline void _encode_destructively(bufferlist& s, bufferlist& bl) 
{
  uint32_t len = s.length();
  _encoderaw(len, bl);
  bl.claim_append(s);
}
inline void _decode(bufferlist& s, bufferlist& bl, int& off)
{
  uint32_t len;
  _decoderaw(len, bl, off);
  s.substr_of(bl, off, len);
  off += len;
}

// base
template<class T>
inline void _encode(const T& t, bufferlist& bl)
{
  _encoderaw(t, bl);
}
template<class T>
inline void _decode(T& t, bufferlist& bl, int& off)
{
  _decoderaw(t, bl, off);
}



#endif
