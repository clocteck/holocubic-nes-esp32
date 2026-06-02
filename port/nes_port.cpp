#include "Arduino.h"

#include <stdarg.h>

static const module_host_api_v1 *s_host = nullptr;

static size_t cstr_len(const char *text)
{
    size_t len = 0;
    if (!text)
    {
        return 0;
    }
    while (text[len])
    {
        len++;
    }
    return len;
}

static int cstr_cmp(const char *a, const char *b)
{
    const unsigned char *pa = (const unsigned char *)(a ? a : "");
    const unsigned char *pb = (const unsigned char *)(b ? b : "");
    while (*pa && *pa == *pb)
    {
        pa++;
        pb++;
    }
    return (int)*pa - (int)*pb;
}

static size_t uint_to_text(unsigned long value, char *out, size_t out_size)
{
    char tmp[24];
    size_t pos = 0;
    size_t len = 0;
    if (!out || out_size == 0)
    {
        return 0;
    }
    do
    {
        tmp[pos++] = (char)('0' + (value % 10UL));
        value /= 10UL;
    } while (value && pos < sizeof(tmp));

    while (len + 1 < out_size && pos > 0)
    {
        out[len++] = tmp[--pos];
    }
    out[len] = '\0';
    return len;
}

static size_t int_to_text(long value, char *out, size_t out_size)
{
    if (!out || out_size == 0)
    {
        return 0;
    }
    if (value < 0)
    {
        out[0] = '-';
        return 1 + uint_to_text((unsigned long)(-value), out + 1, out_size - 1);
    }
    return uint_to_text((unsigned long)value, out, out_size);
}

extern "C" void nes_port_set_host(const module_host_api_v1 *host)
{
    s_host = host;
}

extern "C" const module_host_api_v1 *nes_port_host(void)
{
    return s_host;
}

extern "C" void *nes_port_malloc(size_t size, uint32_t caps)
{
    if (size == 0)
    {
        return nullptr;
    }
    if (s_host && s_host->heap.malloc)
    {
        return s_host->heap.malloc(size, caps);
    }
    return nullptr;
}

extern "C" void *nes_port_calloc(size_t count, size_t size, uint32_t caps)
{
    if (count != 0 && (count * size) / count != size)
    {
        return nullptr;
    }
    if (s_host && s_host->heap.calloc)
    {
        return s_host->heap.calloc(count, size, caps);
    }
    return nullptr;
}

extern "C" void nes_port_free(void *ptr)
{
    if (!ptr)
    {
        return;
    }
    if (s_host && s_host->heap.free)
    {
        s_host->heap.free(ptr);
    }
}

extern "C" uint32_t nes_port_millis(void)
{
    return (s_host && s_host->time.millis) ? s_host->time.millis() : 0;
}

extern "C" uint64_t nes_port_micros(void)
{
    return (s_host && s_host->time.micros) ? s_host->time.micros() : 0;
}

extern "C" void nes_port_delay(uint32_t ms)
{
    if (s_host && s_host->task.delay)
    {
        s_host->task.delay(ms);
    }
    else if (s_host && s_host->time.delay)
    {
        s_host->time.delay(ms);
    }
}

extern "C" uint32_t nes_port_random(void)
{
    static uint32_t state = 0x6D2B79F5u;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state ? state : 0x6D2B79F5u;
}

extern "C" void *nes_port_exec_ptr(const void *ptr)
{
    uintptr_t addr = (uintptr_t)ptr;
    if (addr >= 0x3C000000u && addr < 0x3E000000u)
    {
        addr += 0x06000000u;
    }
    if (addr >= 0x82000000u && addr < 0x84000000u)
    {
        addr -= 0x40000000u;
    }
    return (void *)addr;
}

extern "C" void nes_port_log(const char *text)
{
    if (s_host && s_host->serial.println)
    {
        s_host->serial.println(text ? text : "");
    }
}

extern "C" void nes_port_logf(const char *fmt, ...)
{
    va_list ap;
    if (!fmt)
    {
        return;
    }
    va_start(ap, fmt);
    va_end(ap);
    if (s_host && s_host->serial.print)
    {
        s_host->serial.print(fmt);
    }
}

static uint32_t mode_to_flags(const char *mode)
{
    if (!mode || mode[0] == 'r')
    {
        return MODULE_FILE_READ;
    }
    if (mode[0] == 'a')
    {
        return MODULE_FILE_APPEND | MODULE_FILE_CREATE | MODULE_FILE_WRITE;
    }
    return MODULE_FILE_WRITE | MODULE_FILE_CREATE | MODULE_FILE_TRUNC;
}

extern "C" int nes_port_sd_open(const char *path, const char *mode, void **out_file)
{
    if (!out_file)
    {
        return MODULE_ERR_INVALID_ARG;
    }
    *out_file = nullptr;
    if (!s_host || !s_host->sd.open || !path || !path[0])
    {
        return MODULE_ERR_BAD_STATE;
    }
    return s_host->sd.open(path, mode_to_flags(mode), out_file);
}

extern "C" uint32_t nes_port_file_read(void *file, void *buf, uint32_t len)
{
    size_t got = 0;
    if (!s_host || !s_host->file.read || !file)
    {
        return 0;
    }
    if (s_host->file.read(file, buf, len, &got) != MODULE_OK)
    {
        return 0;
    }
    return (uint32_t)got;
}

extern "C" uint32_t nes_port_file_write(void *file, const void *buf, uint32_t len)
{
    size_t written = 0;
    if (!s_host || !s_host->file.write || !file)
    {
        return 0;
    }
    if (s_host->file.write(file, buf, len, &written) != MODULE_OK)
    {
        return 0;
    }
    return (uint32_t)written;
}

extern "C" int nes_port_file_seek(void *file, int64_t offset, uint32_t mode)
{
    if (!s_host || !s_host->file.seek || !file)
    {
        return 0;
    }
    return s_host->file.seek(file, offset, mode) == MODULE_OK;
}

extern "C" uint64_t nes_port_file_position(void *file)
{
    uint64_t pos = 0;
    if (s_host && s_host->file.position && file)
    {
        (void)s_host->file.position(file, &pos);
    }
    return pos;
}

extern "C" uint64_t nes_port_file_size(void *file)
{
    uint64_t bytes = 0;
    if (s_host && s_host->file.size_bytes && file)
    {
        (void)s_host->file.size_bytes(file, &bytes);
    }
    return bytes;
}

extern "C" int nes_port_file_available(void *file)
{
    size_t available = 0;
    if (s_host && s_host->file.available && file)
    {
        (void)s_host->file.available(file, &available);
    }
    return (int)available;
}

extern "C" int nes_port_file_is_directory(void *file)
{
    int32_t is_dir = 0;
    if (s_host && s_host->file.is_directory && file)
    {
        (void)s_host->file.is_directory(file, &is_dir);
    }
    return is_dir ? 1 : 0;
}

extern "C" void nes_port_file_flush(void *file)
{
    if (s_host && s_host->file.flush && file)
    {
        (void)s_host->file.flush(file);
    }
}

extern "C" void nes_port_file_close(void *file)
{
    if (s_host && s_host->file.close && file)
    {
        (void)s_host->file.close(file);
    }
}

String::String()
    : m_data(nullptr), m_len(0)
{
    assign("", 0);
}

String::String(const char *text)
    : m_data(nullptr), m_len(0)
{
    assign(text ? text : "", cstr_len(text));
}

String::String(char ch)
    : m_data(nullptr), m_len(0)
{
    char tmp[2] = {ch, '\0'};
    assign(tmp, 1);
}

String::String(int value)
    : m_data(nullptr), m_len(0)
{
    char tmp[24];
    const size_t len = int_to_text((long)value, tmp, sizeof(tmp));
    assign(tmp, len);
}

String::String(unsigned int value)
    : m_data(nullptr), m_len(0)
{
    char tmp[24];
    const size_t len = uint_to_text((unsigned long)value, tmp, sizeof(tmp));
    assign(tmp, len);
}

String::String(long value)
    : m_data(nullptr), m_len(0)
{
    char tmp[32];
    const size_t len = int_to_text(value, tmp, sizeof(tmp));
    assign(tmp, len);
}

String::String(unsigned long value)
    : m_data(nullptr), m_len(0)
{
    char tmp[32];
    const size_t len = uint_to_text(value, tmp, sizeof(tmp));
    assign(tmp, len);
}

String::String(const String &other)
    : m_data(nullptr), m_len(0)
{
    assign(other.c_str(), other.length());
}

String::~String()
{
    nes_port_free(m_data);
    m_data = nullptr;
    m_len = 0;
}

String &String::operator=(const char *text)
{
    assign(text ? text : "", cstr_len(text));
    return *this;
}

String &String::operator=(const String &other)
{
    if (this != &other)
    {
        assign(other.c_str(), other.length());
    }
    return *this;
}

String &String::operator+=(const char *text)
{
    const size_t add_len = cstr_len(text);
    char *next = (char *)nes_port_malloc(m_len + add_len + 1, MODULE_HEAP_INTERNAL | MODULE_HEAP_8BIT);
    if (!next)
    {
        return *this;
    }
    for (size_t i = 0; i < m_len; i++)
    {
        next[i] = m_data[i];
    }
    for (size_t i = 0; i < add_len; i++)
    {
        next[m_len + i] = text[i];
    }
    next[m_len + add_len] = '\0';
    nes_port_free(m_data);
    m_data = next;
    m_len += add_len;
    return *this;
}

String &String::operator+=(const String &other)
{
    return (*this += other.c_str());
}

const char *String::c_str() const
{
    return m_data ? m_data : "";
}

size_t String::length() const
{
    return m_len;
}

bool String::startsWith(const char *prefix) const
{
    const char *text = c_str();
    if (!prefix)
    {
        return false;
    }
    for (size_t i = 0; prefix[i]; i++)
    {
        if (text[i] != prefix[i])
        {
            return false;
        }
    }
    return true;
}

bool String::startsWith(const String &prefix) const
{
    return startsWith(prefix.c_str());
}

int String::indexOf(const char *needle) const
{
    const char *text = c_str();
    const size_t needle_len = cstr_len(needle);
    if (!needle || needle_len == 0)
    {
        return 0;
    }
    if (needle_len > m_len)
    {
        return -1;
    }
    for (size_t i = 0; i <= m_len - needle_len; i++)
    {
        size_t j = 0;
        while (j < needle_len && text[i + j] == needle[j])
        {
            j++;
        }
        if (j == needle_len)
        {
            return (int)i;
        }
    }
    return -1;
}

void String::replace(const char *from, const char *to)
{
    const int pos = indexOf(from);
    if (pos < 0)
    {
        return;
    }
    const size_t from_len = cstr_len(from);
    const size_t to_len = cstr_len(to);
    char *next = (char *)nes_port_malloc(m_len - from_len + to_len + 1, MODULE_HEAP_INTERNAL | MODULE_HEAP_8BIT);
    if (!next)
    {
        return;
    }
    for (int i = 0; i < pos; i++)
    {
        next[i] = m_data[i];
    }
    for (size_t i = 0; i < to_len; i++)
    {
        next[pos + i] = to[i];
    }
    for (size_t i = (size_t)pos + from_len; i < m_len; i++)
    {
        next[pos + to_len + (i - ((size_t)pos + from_len))] = m_data[i];
    }
    next[m_len - from_len + to_len] = '\0';
    nes_port_free(m_data);
    m_data = next;
    m_len = m_len - from_len + to_len;
}

void String::remove(size_t index, size_t count)
{
    if (index >= m_len)
    {
        return;
    }
    if (index + count > m_len)
    {
        count = m_len - index;
    }
    for (size_t i = index; i + count <= m_len; i++)
    {
        m_data[i] = m_data[i + count];
    }
    m_len -= count;
}

void String::remove(size_t index)
{
    remove(index, m_len > index ? (m_len - index) : 0);
}

bool String::operator==(const char *rhs) const
{
    return cstr_cmp(c_str(), rhs) == 0;
}

void String::assign(const char *text, size_t len)
{
    char *next = (char *)nes_port_malloc(len + 1, MODULE_HEAP_INTERNAL | MODULE_HEAP_8BIT);
    if (!next)
    {
        return;
    }
    for (size_t i = 0; i < len; i++)
    {
        next[i] = text[i];
    }
    next[len] = '\0';
    nes_port_free(m_data);
    m_data = next;
    m_len = len;
}

String operator+(const String &lhs, const String &rhs)
{
    String out(lhs);
    out += rhs;
    return out;
}

String operator+(const String &lhs, const char *rhs)
{
    String out(lhs);
    out += rhs;
    return out;
}

String operator+(const char *lhs, const String &rhs)
{
    String out(lhs);
    out += rhs;
    return out;
}

File::File()
    : m_file(nullptr)
{
}

File::~File()
{
    close();
}

File::File(File &&other) noexcept
    : m_file(other.m_file)
{
    other.m_file = nullptr;
}

File &File::operator=(File &&other) noexcept
{
    if (this != &other)
    {
        close();
        m_file = other.m_file;
        other.m_file = nullptr;
    }
    return *this;
}

bool File::open(const char *path, const char *mode)
{
    close();
    return nes_port_sd_open(path, mode, &m_file) == MODULE_OK && m_file != nullptr;
}

void File::close()
{
    if (m_file)
    {
        nes_port_file_close(m_file);
        m_file = nullptr;
    }
}

size_t File::read(uint8_t *buf, size_t len)
{
    return nes_port_file_read(m_file, buf, (uint32_t)len);
}

size_t File::write(const uint8_t *buf, size_t len)
{
    return nes_port_file_write(m_file, buf, (uint32_t)len);
}

size_t File::print(const char *text)
{
    return write((const uint8_t *)(text ? text : ""), cstr_len(text));
}

bool File::seek(uint32_t pos)
{
    return seek(pos, fs::SeekSet);
}

bool File::seek(uint32_t pos, fs::SeekMode mode)
{
    return nes_port_file_seek(m_file, pos, (uint32_t)mode) != 0;
}

uint32_t File::position() const
{
    return (uint32_t)nes_port_file_position(m_file);
}

uint32_t File::size() const
{
    return (uint32_t)nes_port_file_size(m_file);
}

int File::available() const
{
    return nes_port_file_available(m_file);
}

bool File::isDirectory() const
{
    return nes_port_file_is_directory(m_file) != 0;
}

void File::flush()
{
    nes_port_file_flush(m_file);
}

void *operator new(size_t size)
{
    return nes_port_malloc(size ? size : 1, MODULE_HEAP_INTERNAL | MODULE_HEAP_8BIT);
}

void *operator new[](size_t size)
{
    return nes_port_malloc(size ? size : 1, MODULE_HEAP_INTERNAL | MODULE_HEAP_8BIT);
}

void operator delete(void *ptr) noexcept
{
    nes_port_free(ptr);
}

void operator delete[](void *ptr) noexcept
{
    nes_port_free(ptr);
}

void operator delete(void *ptr, size_t) noexcept
{
    nes_port_free(ptr);
}

void operator delete[](void *ptr, size_t) noexcept
{
    nes_port_free(ptr);
}

extern "C" size_t strlen(const char *text)
{
    return cstr_len(text);
}
