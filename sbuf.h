/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */

#ifndef SBUF_H
#define SBUF_H

/*
 * sbuf.h:
 *
 * sbuf ("safer buffer") provides a typesafe means to
 * refer to binary data within the context of a C++ computer forensics
 * tool. The sbuf is a const buffer for which the first byte's
 * position is tracked in the "pos0" variable (the position of
 * byte[0]). The buffer may come from a disk, a disk image, or be the
 * result of decompressing or otherwise decoding other data.
 *
 * Created and maintained by Simson Garfinkel, 2007--2012.
 * 2019 - after six years of warning, this is now being tightened up such that
 * there are no public variables. Created and maintained by Simson Garfinkel,
 *
 * 2007--2012, 2019.
 *
 * sbuf_stream is a stream-oriented interface for reading sbuf data.
 *
 * 2020 - complete refactoring for improved performance.
 *
 * 2021 - removed copy constructor. For performance we should never copy, but we
 * can move.
 */

#include <atomic>
#include <cassert>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

#include <sys/mman.h>
#include <unistd.h>

#include "pos0.h"

/*
 * NOTE: The crash identified in November 2019 was because access to
 * *sbuf.buf went beyond buflen. The way around this is to make
 * *sbuf.buf private. Most (but not all) of the accesses to *sbuf.buf
 * appear to be for generating a hash value, so simply providing a
 * hash update function would be a useful addition to the sbuf
 * class. Unfortunately, the crash was not a result of hashing, but
 * result of character-by-character access in scan_wordlist. The bug
 * had been there for more than 7 years without being identified, but
 * was discovered when a compiler was changed...
 *
 * Making *sbuf.buf private would largely fix this problem, but it
 * would require substantial rewriting of several scanners that (at
 * the current time) do not appear to have bugs.
 */

/**
 * \class sbuf_t
 * This class describes the search buffer.
 * The accessors are safe so that no buffer overflow can happen.
 * Integer readers may throw sbuf_bounds_exception.
 *
 * This structure actually holds the data.
 * We use a pos0_t to maintain the address of the first byte.
 *
 * There are lots of ways for allocating an sbuf_t:
 * - map from a file.
 * - set from a block of memory.
 * - a subset of an existing sbuf_t (sbuf+10 gives you 10 bytes in, and
 * therefore 10 bytes shorter)
 *
 * The subf_t class remembers how the sbuf_t was allocated and
 * automatically frees whatever resources are needed when it is freed.
 *
 * \warning DANGER: You must delete sbuf_t structures First-In,
 * Last-out, otherwise bad things can happen. (For example, if you
 * make a subset sbuf_t from a mapped file and unmap the file, the
 * subset will now point to unallocated memory.)
 */
class sbuf_t {
    /* The private structures keep track of memory management */
    static size_t min(size_t a, size_t b) { return a < b ? a : b; }
    int fd{0}; /* file this came from if mmapped file */

public:
    const bool should_unmap{false}; /* munmap buffer when done */
    const bool should_free{false};  /* should buf be freed when this sbuf is deleted? */
    const bool should_close{false}; /* close(fd) when done. */
    const uint64_t page_number{0};  /* from iterator when sbuf is created */
    const pos0_t pos0{};            /* the path of buf[0] */
private:
    const sbuf_t* parent{nullptr}; // parent sbuf references data in another.
    mutable std::mutex Mhash{};    // mutext for hashing
    mutable std::string hash_{};   // the hash of the sbuf data, or "" if it
                                   // hasn't been hashed yet
public:
    mutable std::atomic<int> children{0}; // number of child sbufs; incremented
                                          // when data in *buf is used by a child
    const unsigned int depth() const { return pos0.depth; }
#ifdef PRIVATE_SBUF_BUF
private: // one day
#else
public:
#endif
    /**
     * \deprecated
     * This field will be private in a future release of \b bulk_extractor.
     */
    const uint8_t* buf{nullptr}; /* start of the buffer */
public:
    const size_t bufsize{0};  /* size of the buffer */
    const size_t pagesize{0}; /* page data; the rest is the 'margin'. pagesize <= bufsize */

private:
    void release();                                 // release allocated storage
    sbuf_t(const sbuf_t& that) = delete;            // default copy is not implemented
    sbuf_t& operator=(const sbuf_t& that) = delete; // default assignment not implemented
public:
    /** Make an empty sbuf.
        It's used for situations where an sbuf is needed but not referenced */
    explicit sbuf_t() {}

    /****************************************************************
     *** Child allocators --- allocate an sbuf from another sbuf
     ****************************************************************/

    /** Move constructor is properly implemented. */
    sbuf_t(sbuf_t&& that) noexcept
        : page_number(that.page_number), pos0(that.pos0), parent(that.parent), buf(that.buf), bufsize(that.bufsize),
          pagesize(that.pagesize) {
        parent->del_child(that);
        parent->add_child(*this);
    }

    /**
     * Make an sbuf from a parent but with a different path.
     */
    explicit sbuf_t(const pos0_t& that_pos0, const sbuf_t& that_sbuf)
        : page_number(that_sbuf.page_number), pos0(that_pos0), parent(that_sbuf.highest_parent()), buf(that_sbuf.buf),
          bufsize(that_sbuf.bufsize), pagesize(that_sbuf.pagesize) {
        parent->add_child(*this);
    }

    /** Allocate from within an existing sbuf.
     * The allocated buf MUST be freed before the source, since no copy is
     * made...
     */
    explicit sbuf_t(const sbuf_t& sbuf, size_t off, size_t len)
        : page_number(sbuf.page_number), pos0(sbuf.pos0 + off), parent(sbuf.highest_parent()), buf(sbuf.buf + off),
          bufsize(off + len < sbuf.bufsize ? len : sbuf.bufsize - off),
          pagesize(off + len < sbuf.bufsize ? len : sbuf.bufsize - off) {
        parent->add_child(*this);
    };

    /**
     * make an sbuf from a parent but with an indent. This should be folded into
     * the above.
     */
    explicit sbuf_t(const sbuf_t& that_sbuf, size_t off)
        : page_number(that_sbuf.page_number), pos0(that_sbuf.pos0 + off), parent(that_sbuf.highest_parent()),
          buf(that_sbuf.buf + off), bufsize(that_sbuf.bufsize > off ? that_sbuf.bufsize - off : 0),
          pagesize(that_sbuf.pagesize > off ? that_sbuf.pagesize - off : 0) {}

    /****************************************************************
     *** Allocators that allocate from memory not already in an sbuf.
     ****************************************************************/

    /* Allocators */
    /** Allocate a new buffer of a given size for filling.
     * This is the one case where buf is written into...
     * This should probably be a subclass mutable_sbuf_t() for clarity.
     */

    /* Allocate from an existing buffer, optionally disposing of the memory
     * unmapping or freeing, and optionally closing an associated file
     * descriptor.
     */
    explicit sbuf_t(const pos0_t& pos0_, const uint8_t* buf_, size_t bufsize_, size_t pagesize_, int fd_,
                    bool should_unmap_, bool should_free_, bool should_close_)
        : fd(fd_), should_unmap(should_unmap_), should_free(should_free_), should_close(should_close_), pos0(pos0_),
          buf(buf_), bufsize(bufsize_), pagesize(min(pagesize_, bufsize_)){};

    /**
     * the + operator returns a new sbuf that is i bytes in and, therefore, i
     * bytes smaller. Note:
     * 1. We assume that pagesize is always smaller than or equal to bufsize.
     * 2. The child sbuf uses the parent's memory. If the parent gets deleted,
     * the child points to invalid data.
     *
     * 3. If i is bigger than pagesize, then an sbuf is returned with
     *    0 bytes in the page and all of the margin.
     *
     *    (Because we won't return what's in the margin as page data.)
     */
    sbuf_t operator+(size_t off) const { return sbuf_t(*this, off); }

    /* Allocate a sbuf from a file mapped into memory. The sbuf must be deleted
     */
    static sbuf_t* map_file(const std::filesystem::path fname); // map a sbuf from a file, or throw exception
    static sbuf_t* map_file(const std::filesystem::path fname, int fd,
                            bool should_close);                   // if file is already opened
    inline static const std::string U10001C = "\xf4\x80\x80\x9c"; // default delimeter character in bulk_extractor
    static std::string map_file_delimiter;                        // character placed
    static void set_map_file_delimiter(const std::string new_delim) { map_file_delimiter = new_delim; }

    /* Allocate a sbuf by copying memory in the current sbuf. The sbuf must be
     * deleted. */
    sbuf_t* sbuf_malloc(size_t offset, size_t len) const;

    /* Allocate an sbuf from a null-terminated string. Used for debugging mostly
     */
    explicit sbuf_t(const char* str)
        : buf(reinterpret_cast<const uint8_t*>(str)), bufsize(strlen(str)), pagesize(strlen(str)){};

    /* Properties */
    size_t size() const { return bufsize; }                                // return the number of bytes
    size_t left(size_t n) const { return n < bufsize ? bufsize - n : 0; }; // how much space is left at n

    const sbuf_t* highest_parent() const; // returns the parent of the parent...
    void add_child(const sbuf_t& child) const { children += 1; }
    void del_child(const sbuf_t& child) const {
        children -= 1;
        assert(children >= 0);
    }

    /** Find the offset of a byte */
    size_t offset(const uint8_t* loc) const {
        if (loc < buf) { return 0; }
        if (loc > buf + bufsize) { return bufsize; }
        return loc - buf;
    }

    /**
     * asString - returns the sbuf as a string
     */

    std::string asString() const { return std::string((reinterpret_cast<const char*>(buf)), bufsize); }

    /* return true if the sbuf consists solely of ngrams */
    size_t find_ngram_size(size_t max_ngram) const;

    /* get the next line line from the sbuf.
     * @param pos  - on entry, current position. On exit, new position.
     *               pos[0] is the start of a line
     * @param start - the start of the line, from the start of the sbuf.
     * @param len   - the length of the line; does not include the \n at the
     * end.
     * @return true - a line was found; false - a line was not found
     */
    bool getline(size_t& pos, size_t& line_start, size_t& line_len) const {
        /* Scan forward until pos is at the beginning of a line */
        if (pos >= this->pagesize) return false;
        if (pos > 0) {
            while ((pos < this->pagesize) && (*this)[pos - 1] != '\n') { ++(pos); }
            if (pos >= this->pagesize) return false; // didn't find another start of a line
        }
        line_start = pos;
        /* Now scan to end of the line, or the end of the buffer */
        while (++pos < this->pagesize) {
            if ((*this)[pos] == '\n') { break; }
        }
        line_len = (pos - line_start);
        return true;
    }

    /****************************************************************
     *** range_exception_t
     *** An sbuf_range_exception object is thrown if the attempted sbuf access
     *is out of range.
     ****************************************************************/
    /**
     * sbuf_t raises an sbuf_range_exception when an attempt is made to read
     * past the end of buf.
     */
    class range_exception_t : public std::exception {
    public:
        virtual const char* what() const throw() { return "Error: Read past end of sbuf"; }
    };

    /****************************************************************
     *** The following get functions read integer and string types
     *** or else throw an sbuf_range_exception if out of range.
     ****************************************************************/

    /* Search functions --- memcmp at a particular location */
    int memcmp(const uint8_t* cbuf, size_t at, size_t len) const {
        if (left(at) < len) throw sbuf_t::range_exception_t();
        return ::memcmp(this->buf + at, cbuf, len);
    }

    /**
     * \name unsigned int Intel (littel-endian) readers
     * @{
     * these get functions safely return an unsigned integer value for the
     * offset of i, in Intel (little-endian) byte order or else throw
     * sbuf_range_exception if out of range.
     *
     * These should be used instead of buf[i]
     */
    uint8_t get8u(size_t i) const {
        if (i + 1 > bufsize) throw sbuf_t::range_exception_t();
        return this->buf[i];
    }

    uint16_t get16u(size_t i) const {
        if (i + 2 > bufsize) throw sbuf_t::range_exception_t();
        return 0 | (uint16_t)(this->buf[i + 0] << 0) | (uint16_t)(this->buf[i + 1] << 8);
    }

    uint32_t get32u(size_t i) const {
        if (i + 4 > bufsize) throw sbuf_t::range_exception_t();
        return 0 | (uint32_t)(this->buf[i + 0] << 0) | (uint32_t)(this->buf[i + 1] << 8) |
               (uint32_t)(this->buf[i + 2] << 16) | (uint32_t)(this->buf[i + 3] << 24);
    }

    uint64_t get64u(size_t i) const {
        if (i + 8 > bufsize) throw sbuf_t::range_exception_t();
        return 0 | ((uint64_t)(this->buf[i + 0]) << 0) | ((uint64_t)(this->buf[i + 1]) << 8) |
               ((uint64_t)(this->buf[i + 2]) << 16) | ((uint64_t)(this->buf[i + 3]) << 24) |
               ((uint64_t)(this->buf[i + 4]) << 32) | ((uint64_t)(this->buf[i + 5]) << 40) |
               ((uint64_t)(this->buf[i + 6]) << 48) | ((uint64_t)(this->buf[i + 7]) << 56);
    }

    /** @} */

    /**
     * \name unsigned int Motorola (big-endian) readers
     * @{
     * these get functions safely return an unsigned integer value for the
     * offset of i, in Motorola (big-endian) byte order or else throw
     * sbuf_range_exception if out of range.
     */

    /**
     * \name signed int Intel (little-endian) readers
     * @{
     * these get functions safely return a signed integer value for the offset
     * of i, in Intel (little-endian) byte order or else throw
     * sbuf_range_exception if out of range.
     */
    uint8_t get8uBE(size_t i) const {
        if (i + 1 > bufsize) throw sbuf_t::range_exception_t();
        return this->buf[i];
    }

    uint16_t get16uBE(size_t i) const {
        if (i + 2 > bufsize) throw sbuf_t::range_exception_t();
        return 0 | (uint16_t)(this->buf[i + 1] << 0) | (uint16_t)(this->buf[i + 0] << 8);
    }

    uint32_t get32uBE(size_t i) const {
        if (i + 4 > bufsize) throw sbuf_t::range_exception_t();
        return 0 | (uint32_t)(this->buf[i + 3] << 0) | (uint32_t)(this->buf[i + 2] << 8) |
               (uint32_t)(this->buf[i + 1] << 16) | (uint32_t)(this->buf[i + 0] << 24);
    }

    uint64_t get64uBE(size_t i) const {
        if (i + 8 > bufsize) throw sbuf_t::range_exception_t();
        return 0 | ((uint64_t)(this->buf[i + 7]) << 0) | ((uint64_t)(this->buf[i + 6]) << 8) |
               ((uint64_t)(this->buf[i + 5]) << 16) | ((uint64_t)(this->buf[i + 4]) << 24) |
               ((uint64_t)(this->buf[i + 3]) << 32) | ((uint64_t)(this->buf[i + 2]) << 40) |
               ((uint64_t)(this->buf[i + 1]) << 48) | ((uint64_t)(this->buf[i + 0]) << 56);
    }

    /** @} */

    /**
     * \name signed int Motorola (big-endian) readers
     * @{
     * these get functions safely return a signed integer value for the offset
     * of i, in Motorola (big-endian) byte order or else throw
     * sbuf_range_exception if out of range.
     */
    /** @} */

    /**
     * some get functions take byte_order_t as a specifier to indicate which
     * endian format to use.
     */
    typedef enum { BO_LITTLE_ENDIAN = 0, BO_BIG_ENDIAN = 1 } byte_order_t;

    /**
     * \name unsigned int, byte-order specified readers
     * @{
     * these get functions safely return an unsigned integer value for the
     * offset of i, in the byte order of your choice or else throw
     * sbuf_range_exception if out of range.
     */
    uint8_t get8u(size_t i, sbuf_t::byte_order_t bo) const { return bo == BO_LITTLE_ENDIAN ? get8u(i) : get8uBE(i); }

    uint16_t get16u(size_t i, sbuf_t::byte_order_t bo) const {
        return bo == BO_LITTLE_ENDIAN ? get16u(i) : get16uBE(i);
    }

    uint32_t get32u(size_t i, sbuf_t::byte_order_t bo) const {
        return bo == BO_LITTLE_ENDIAN ? get32u(i) : get32uBE(i);
    }

    uint64_t get64u(size_t i, sbuf_t::byte_order_t bo) const {
        return bo == BO_LITTLE_ENDIAN ? get64u(i) : get64uBE(i);
    }

    /** @} */

    /**
     * Signed get interfaces simply call the unsigned interfaces and
     * the return gets cast.
     */
     int8_t get8i(size_t i)    const { return get8u(i);}
    int16_t get16i(size_t i)   const { return get16u(i);}
    int32_t get32i(size_t i)   const { return get32u(i);}
    int64_t get64i(size_t i)   const { return get64u(i);}
     int8_t get8iBE(size_t i)  const { return get8uBE(i);}
    int16_t get16iBE(size_t i) const { return get16uBE(i);}
    int32_t get32iBE(size_t i) const { return get32uBE(i);}
    int64_t get64iBE(size_t i) const { return get64uBE(i);}



    /**
     * \name signed int, byte-order specified readers
     * @{
     * these get functions safely return a signed integer value for the offset
     * of i, in the byte order of your choice or else throw sbuf_range_exception
     * if out of range.
     */
    int8_t get8i(size_t i, sbuf_t::byte_order_t bo) const { return bo == BO_LITTLE_ENDIAN ? get8u(i) : get8uBE(i); }

    int16_t get16i(size_t i, sbuf_t::byte_order_t bo) const { return bo == BO_LITTLE_ENDIAN ? get16u(i) : get16uBE(i); }

    int32_t get32i(size_t i, sbuf_t::byte_order_t bo) const { return bo == BO_LITTLE_ENDIAN ? get32u(i) : get32uBE(i); }

    int64_t get64i(size_t i, sbuf_t::byte_order_t bo) const { return bo == BO_LITTLE_ENDIAN ? get64u(i) : get64uBE(i); }
    /** @} */

    /**
     * \name string readers
     * @{
     * These get functions safely read string
     */
    void getUTF8(size_t i, size_t num_octets_requested, std::string& utf8_string) const;
    void getUTF8(size_t i, std::string& utf8_string) const;
    /** @} */

    /**
     * \name wstring readers
     * @{
     * These get functions safely read wstring
     */
    void getUTF16(size_t i, size_t num_code_units_requested, std::wstring& utf16_string) const;
    void getUTF16(size_t i, std::wstring& utf16_string) const;
    void getUTF16(size_t i, size_t num_code_units_requested, byte_order_t bo, std::wstring& utf16_string) const;
    void getUTF16(size_t i, byte_order_t bo, std::wstring& utf16_string) const;
    /** @} */

    /**
     * The [] operator safely returns what's at index [i] or else returns 0 if
     * out of range. We made a decision that this would not throw the exception
     * Notice that we don't need to check to see if i<0 because i is unsigned.
     */
    uint8_t operator[](size_t i) const { return (i < bufsize) ? buf[i] : 0; }

    /**
     * Find the next occurance of a character in the buffer
     * starting at a given point.
     * return -1 if there is none to find.
     */
    ssize_t find(uint8_t ch, size_t start = 0) const {
        for (; start < pagesize; start++) {
            if (buf[start] == ch) return start;
        }
        return -1;
    }

    /**
     * Find the next occurance of a char* string in the buffer
     * starting at a give point.
     * Return offset or -1 if there is none to find.
     * This would benefit from a boyer-Moore implementation
     */
    ssize_t find(const char* str, size_t start = 0) const {
        if (str[0] == 0) return -1; // nothing to search for

        for (; start < pagesize; start++) {
            const uint8_t* p = (const uint8_t*)memchr(buf + start, str[0], bufsize - start);
            if (p == 0) return -1; // first character not present,
            size_t loc = p - buf;
            for (size_t i = 0; loc + i < bufsize && str[i]; i++) {
                if (buf[loc + i] != str[i]) break;
                if (str[i + 1] == 0) return loc; // next char is null, so we are found!
            }
            start = loc + 1; // advance to character after found character
        }
        return -1;
    }

    const std::string substr(size_t loc, size_t len) const; /* make a substring */
    bool is_constant(size_t loc, size_t len,
                     uint8_t ch) const; // verify that it's constant
    bool is_constant(uint8_t ch) const { return is_constant(0, this->pagesize, ch); }

    // Return a pointer to a structure contained within the sbuf if there is
    // room, otherwise return a null pointer.
    template <class TYPE> const TYPE* get_struct_ptr(uint32_t pos) const {
        if (pos + sizeof(TYPE) <= bufsize) { return reinterpret_cast<const TYPE*>(buf + pos); }
        return NULL;
    }

    // Return the hash of the buffer's contents. The hash function doesn't
    // matter; we use SHA1 currently
    std::string hash() const;

    /**
     * These are largely for debugging, but they also support the BEViewer.
     * Dump the sbuf to a stream.
     */
    void raw_dump(std::ostream& os, uint64_t start, uint64_t len) const;
    void raw_dump(int fd, uint64_t start,
                  uint64_t len) const; // writes to a raw file descriptor
    void hex_dump(std::ostream& os, uint64_t start, uint64_t len) const;
    void hex_dump(std::ostream& os) const;                /* dump all */
    ssize_t write(int fd, size_t loc, size_t len) const;  /* write to a file descriptor, returns # bytes written */
    ssize_t write(FILE* f, size_t loc, size_t len) const; /* write to a file descriptor, returns # bytes written */
    ssize_t write(std::ostream& os) const;                // write to an os
    void write(std::filesystem::path path) const;         // throws runtime_error if it can't write

    virtual ~sbuf_t() {
        if (children != 0) { std::cerr << "error: sbuf children=" << children << "\n"; }
        if (parent) parent->del_child(*this);

#ifdef HAVE_MMAP
        if (should_unmap && buf) { munmap((void*)buf, bufsize); }
#endif
        if (should_close && fd > 0) { ::close(fd); }
        if (should_free && buf) { free((void*)buf); }
    }
};

std::ostream& operator<<(std::ostream& os, const sbuf_t& sbuf);

#endif
