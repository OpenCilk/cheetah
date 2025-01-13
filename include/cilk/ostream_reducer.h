#ifndef _OSTREAM_REDUCER_H
#define _OSTREAM_REDUCER_H

#ifdef __cplusplus

#include <ostream>
#include <sstream>

/* Adapted from Intel Cilk Plus */

namespace cilk {

template<typename Char, typename Traits>
class ostream_view : public std::basic_ostream<Char, Traits>
{
    typedef std::basic_ostream<Char, Traits>  base;
    typedef std::basic_ostream<Char, Traits>  ostream_type;

    // A non-leftmost view is associated with a private string buffer. (The
    // leftmost view is associated with the buffer of the reducer's associated
    // ostream, so its private buffer is unused.)
    //
    std::basic_stringbuf<Char, Traits> m_buffer;

public:
    void reduce(ostream_view* other)
    {
        // Writing an empty buffer results in failure. Testing `sgetc()` is the
        // easiest way of checking for an empty buffer.
        if (other->m_buffer.sgetc() != Traits::eof()) {
            *this << (&other->m_buffer);
        }
    }

    static void reduce(void *left_v, void *right_v) {
      ostream_view<Char, Traits> *left =
        static_cast<ostream_view<Char, Traits> *>(left_v);
      ostream_view<Char, Traits> *right =
        static_cast<ostream_view<Char, Traits> *>(right_v);
      left->reduce(right);
      right->~ostream_view();
    }

    static void identity(void *view) {
      new (view) ostream_view<Char, Traits>();
    }

    /** Non-leftmost (identity) view constructor. The view is associated with
     *  its internal buffer. Required by @ref monoid_base.
     */
    ostream_view() : base(&m_buffer) {}

    /** Leftmost view constructor. The view is associated with an existing
     *  ostream.
     */
    ostream_view(const ostream_type& os) : base(0)
    {
        base::rdbuf(os.rdbuf());       // Copy stream buffer
        base::flags(os.flags());       // Copy formatting flags
        base::setstate(os.rdstate());  // Copy error state
    }

};

template<typename Char, typename Traits = std::char_traits<Char>>
  using ostream_reducer = ostream_view<Char, Traits>
    _Hyperobject(&ostream_view<Char, std::char_traits<Char>>::identity,
                 &ostream_view<Char, std::char_traits<Char>>::reduce);

} // namespace cilk

#endif // __cplusplus

#endif // _OSTREAM_REDUCER_H
