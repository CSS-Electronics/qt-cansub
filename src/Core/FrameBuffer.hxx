#ifndef CANSUB_QT_FRAMEBUFFER_HXX
#define CANSUB_QT_FRAMEBUFFER_HXX

#include <cstddef>
#include <memory>

/**
 * Simple data class helper to track a continuous segment of data.
 *
 * - Tracks length of available data
 * - Allows for multi-byte appending
 * - Allows for multi-byte consumption
 * - Has a peek functionality
 * - Can be compacted once empty
 */
struct FrameBuffer
{
    explicit FrameBuffer(std::size_t capacity);

    /**
     * Append a number of bytes to the internal buffer, moving the write pointer forward
     * @param src Location to read data from
     * @param length Number of bytes to copy
     * @return True on success, false otherwise
     */
    bool append(const std::byte* src, std::size_t length);

    /**
     * Get the number of available bytes in the buffer
     * @return The number of bytes available for read from the buffer
     */
    [[nodiscard]] std::size_t available() const;

    /**
     * Peek at the next byte in the buffer without moving the read pointer
     * @return Get the next byte in the buffer
     */
    [[nodiscard]] std::byte const* peek() const;

    /**
     * Consume n bytes from the buffer, moving the read pointer forward
     * @param n Number of bytes to consume
     */
    void consume(std::size_t n);

    /**
     * Reset the buffer if there is no stored data
     */
    void recycle();

    /**
      * Discard all buffered data and reset the buffer, regardless of content
      */
    void clear();

private:
    /**
     * Compact the buffer by moving all data down to the beginning of the internal buffer
     */
    void compact();

    /* Internal data storage */
    std::unique_ptr<std::byte[]> data;
    std::size_t capacity;
    std::size_t readPosition = 0;
    std::size_t writePosition = 0;
};

#endif //CANSUB_QT_FRAMEBUFFER_HXX
