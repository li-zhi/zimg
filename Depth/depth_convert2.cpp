#include <algorithm>
#include <cstdint>
#include <utility>
#include "Common/align.h"
#include "Common/except.h"
#include "Common/linebuffer.h"
#include "Common/pixel.h"
#include "depth_convert2.h"
#include "quantize.h"

namespace zimg {;
namespace depth {;

namespace {;

template <class T>
void integer_to_float(const void *src, void *dst, float scale, float offset, unsigned left, unsigned right)
{
	const T *src_p = reinterpret_cast<const T *>(src);
	float *dst_p = reinterpret_cast<float *>(dst);

	std::transform(src_p + left, src_p + right, dst_p + left, [=](T x){ return static_cast<float>(x) * scale + offset; });
}

void half_to_float_n(const void *src, void *dst, unsigned left, unsigned right)
{
	const uint16_t *src_p = reinterpret_cast<const uint16_t *>(src);
	float *dst_p = reinterpret_cast<float *>(dst);

	std::transform(src_p + left, src_p + right, dst_p + left, half_to_float);
}

void float_to_half_n(const void *src, void *dst, unsigned left, unsigned right)
{
	const float *src_p = reinterpret_cast<const float *>(src);
	uint16_t *dst_p = reinterpret_cast<uint16_t *>(dst);

	std::transform(src_p + left, src_p + right, dst_p + left, float_to_half);
}

depth_convert_func select_depth_convert_func(const PixelFormat &format_in, const zimg::PixelFormat &format_out)
{
	PixelType type_in = format_in.type;
	PixelType type_out = format_out.type;

	if (type_in == PixelType::HALF)
		type_in = PixelType::FLOAT;
	if (type_out == PixelType::HALF)
		type_out = PixelType::FLOAT;

	if (type_in == PixelType::BYTE && type_out == PixelType::FLOAT)
		return integer_to_float<uint8_t>;
	else if (type_in == PixelType::WORD && type_out == PixelType::FLOAT)
		return integer_to_float<uint16_t>;
	else if (type_in == PixelType::FLOAT && type_out == PixelType::FLOAT)
		return nullptr;
	else
		throw error::InternalError{ "no conversion between pixel types" };
}


class ConvertToFloat : public ZimgFilter {
	depth_convert_func m_func;
	depth_f16c_func m_f16c;

	PixelType m_pixel_in;
	PixelType m_pixel_out;
	float m_scale;
	float m_offset;

	unsigned m_width;
	unsigned m_height;
public:
	ConvertToFloat(depth_convert_func func, depth_f16c_func f16c, unsigned width, unsigned height,
	               const PixelFormat &pixel_in, const PixelFormat &pixel_out) :
		m_func{ func },
		m_f16c{ f16c },
		m_pixel_in{ pixel_in.type },
		m_pixel_out{ pixel_out.type },
		m_scale{},
		m_offset{},
		m_width{ width },
		m_height{ height }
	{
		if (pixel_in == pixel_out)
			throw error::InternalError{ "cannot perform no-op conversion" };
		if (f16c && pixel_in.type != PixelType::HALF && pixel_out.type != PixelType::HALF)
			throw error::InternalError{ "cannot provide f16c function for non-HALF types" };
		if (pixel_out.type != PixelType::HALF && pixel_out.type != PixelType::FLOAT)
			throw error::InternalError{ "DepthConvert only converts to floating point types" };

		bool is_integer = (pixel_in.type == PixelType::BYTE || pixel_in.type == PixelType::WORD);

		int32_t range = is_integer ? integer_range(pixel_in.depth, pixel_in.fullrange, pixel_in.chroma) : 1;
		int32_t offset = is_integer ? integer_offset(pixel_in.depth, pixel_in.fullrange, pixel_in.chroma) : 0;

		m_scale = static_cast<float>(1.0 / range);
		m_offset = static_cast<float>(-offset * (1.0 / range));
	}

	ZimgFilterFlags get_flags() const override
	{
		ZimgFilterFlags flags{};

		flags.same_row = true;
		flags.in_place = (pixel_size(m_pixel_in) == pixel_size(m_pixel_out));

		return flags;
	}

	image_attributes get_image_attributes() const override
	{
		return{ m_width, m_height, m_pixel_out };
	}

	size_t get_tmp_size(unsigned left, unsigned right) const override
	{
		size_t size = 0;

		if (m_func && m_f16c) {
			unsigned pixel_align = ALIGNMENT / std::min(pixel_size(m_pixel_in), pixel_size(m_pixel_out));

			left = mod(left, pixel_align);
			right = align(right, pixel_align);

			size = (right - left) * sizeof(float);
		}

		return size;
	}

	void process(void *, const ZimgImageBufferConst &src, const ZimgImageBuffer &dst, void *tmp, unsigned i, unsigned left, unsigned right) const override
	{
		const char *src_line = LineBuffer<const char>{ src }[i];
		char *dst_line = LineBuffer<char>{ dst }[i];

		unsigned pixel_align = ALIGNMENT / std::min(pixel_size(m_pixel_in), pixel_size(m_pixel_out));
		unsigned line_base = mod(left, pixel_align);

		src_line += pixel_size(m_pixel_in) * line_base;
		dst_line += pixel_size(m_pixel_out) * line_base;

		left -= line_base;
		right -= line_base;

		if (m_func && m_f16c) {
			m_func(src_line, tmp, m_scale, m_offset, left, right);
			m_f16c(tmp, dst_line, left, right);
		} else if (m_func) {
			m_func(src_line, dst_line, m_scale, m_offset, left, right);
		} else {
			m_f16c(src_line, dst_line, left, right);
		}
	}
};

} // namespace


IZimgFilter *create_convert_to_float(unsigned width, unsigned height, const PixelFormat &pixel_in, const PixelFormat &pixel_out, CPUClass cpu)
{
	depth_convert_func func = nullptr;
	depth_f16c_func f16c = nullptr;
	bool needs_f16c = (pixel_in.type == PixelType::HALF || pixel_out.type == PixelType::HALF);

	if (!func)
		func = select_depth_convert_func(pixel_in, pixel_out);

	if (needs_f16c) {
		if (!f16c && pixel_in.type == zimg::PixelType::HALF)
			f16c = half_to_float_n;
		if (!f16c && pixel_out.type == zimg::PixelType::HALF)
			f16c = float_to_half_n;
	}

	return new ConvertToFloat{ func, f16c, width, height, pixel_in, pixel_out };
}

} // namespace depth
} // namespace zimg
