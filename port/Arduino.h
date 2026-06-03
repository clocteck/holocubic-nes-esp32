#pragma once

#include <stddef.h>
#include <stdint.h>

#include "module_abi.h"
#include "nes_port.h"

#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

#ifndef MOD_IRAM_ATTR
#define MOD_IRAM_ATTR __attribute__((section(".mod_iram"), noinline, used))
#endif

#ifndef DRAM_ATTR
#define DRAM_ATTR
#endif

#ifndef DMA_ATTR
#define DMA_ATTR
#endif

#ifndef FILE_READ
#define FILE_READ "r"
#endif

#ifndef FILE_WRITE
#define FILE_WRITE "w"
#endif

#ifndef FILE_APPEND
#define FILE_APPEND "a"
#endif

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef void *TaskHandle_t;

namespace fs
{
enum SeekMode
{
    SeekSet = 0,
    SeekCur = 1,
    SeekEnd = 2,
};
} // namespace fs

class String
{
public:
    String();
    String(const char *text);
    String(char ch);
    String(int value);
    String(unsigned int value);
    String(long value);
    String(unsigned long value);
    String(const String &other);
    ~String();

    String &operator=(const char *text);
    String &operator=(const String &other);
    String &operator+=(const char *text);
    String &operator+=(const String &other);

    const char *c_str() const;
    size_t length() const;
    bool startsWith(const char *prefix) const;
    bool startsWith(const String &prefix) const;
    int indexOf(const char *needle) const;
    void replace(const char *from, const char *to);
    void remove(size_t index, size_t count);
    void remove(size_t index);

    bool operator==(const char *rhs) const;
    bool operator!=(const char *rhs) const { return !(*this == rhs); }
    explicit operator bool() const { return length() > 0; }

private:
    char *m_data;
    size_t m_len;

    void assign(const char *text, size_t len);
};

String operator+(const String &lhs, const String &rhs);
String operator+(const String &lhs, const char *rhs);
String operator+(const char *lhs, const String &rhs);

class File
{
public:
    File();
    ~File();
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    File(File &&other) noexcept;
    File &operator=(File &&other) noexcept;

    bool open(const char *path, const char *mode);
    void close();
    size_t read(uint8_t *buf, size_t len);
    size_t write(const uint8_t *buf, size_t len);
    size_t print(const char *text);
    bool seek(uint32_t pos);
    bool seek(uint32_t pos, fs::SeekMode mode);
    uint32_t position() const;
    uint32_t size() const;
    int available() const;
    bool isDirectory() const;
    void flush();
    explicit operator bool() const { return m_file != nullptr; }

private:
    void *m_file;
};
