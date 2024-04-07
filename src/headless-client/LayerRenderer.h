#ifndef HEADLESS_CLIENT_LAYER_RENDERER_H
#define HEADLESS_CLIENT_LAYER_RENDERER_H

#include "Iterators.h"
#include "libclient/drawdance/canvasstate.h"
#include <expected>
#include <iostream>
#include <string>
#include <vector>
extern "C" {
#include <dpengine/layer_group.h>
#include <dpengine/layer_list.h>
#include <dpengine/layer_props.h>
#include <dpengine/layer_props_list.h>
}

struct ParsedTitle {
	bool is_hidden = false;
	bool is_exported = false;
	bool is_meta = false;
	bool is_completed = false;
	bool is_community = false;
	int priority = 5;
	std::string_view faction;
	std::string_view name;

	bool shouldSkip() const { return is_hidden || is_meta; }
};

ParsedTitle parse_layer_title(std::string_view title);

struct Layer {
	Layer() = default;

	Layer(ParsedTitle title, DP_LayerListEntry *lle, DP_LayerProps *lp)
		: m_title(title)
		, m_lle(lle)
		, m_lp(lp)
	{
		if(!lle)
			return;
		if(isGroup()) {
			DP_LayerList *ll = DP_layer_group_children_noinc(
				DP_layer_list_entry_group_noinc(lle));
			DP_LayerPropsList *lpl = DP_layer_props_children_noinc(lp);
			if(ll && lpl)
				m_children = fromLayerList(ll, lpl);
		} else {
			m_lc = DP_layer_list_entry_content_noinc(lle);
		}
	}

	Layer(Layer &&other) noexcept { *this = std::move(other); }

	Layer &operator=(Layer &&other) noexcept
	{
		if(&other == this)
			return *this;

		std::swap(m_title, other.m_title);
		std::swap(m_lle, other.m_lle);
		std::swap(m_lc, other.m_lc);
		std::swap(m_lp, other.m_lp);
		std::swap(m_renderedLayer, other.m_renderedLayer);
		std::swap(m_exclusionMask, other.m_exclusionMask);
		std::swap(m_children, other.m_children);

		return *this;
	}

	~Layer()
	{
		if(m_renderedLayer)
			DP_transient_layer_content_decref(m_renderedLayer);
		if(m_exclusionMask)
			DP_transient_layer_content_decref(m_exclusionMask);
	}

	DP_TransientLayerContent *getOrCreateRenderedTLC(int width, int height)
	{
		if(!m_renderedLayer)
			m_renderedLayer =
				DP_transient_layer_content_new_init(width, height, nullptr);
		return m_renderedLayer;
	}

	DP_TransientLayerContent *getOrCreateExclusionTLC(int width, int height)
	{
		if(!m_exclusionMask)
			m_exclusionMask =
				DP_transient_layer_content_new_init(width, height, nullptr);
		return m_exclusionMask;
	}

	static std::vector<Layer>
	fromLayerList(DP_LayerList *ll, DP_LayerPropsList *lpl);

	const ParsedTitle &parsedTitle() const { return m_title; }

	bool isGroup() const { return DP_layer_list_entry_is_group(m_lle); }

	uint16_t opacity() const { return DP_layer_props_opacity(m_lp); }

	int blendMode() const { return DP_layer_props_blend_mode(m_lp); }

	ParsedTitle m_title;
	DP_LayerListEntry *m_lle = nullptr;
	DP_LayerContent *m_lc = nullptr;
	DP_LayerProps *m_lp = nullptr;
	DP_TransientLayerContent *m_renderedLayer = nullptr;
	DP_TransientLayerContent *m_exclusionMask = nullptr;
	std::vector<Layer> m_children;
};

template <class Func>
void forEachPixel(DP_TransientLayerContent *tlc, Func f)
	requires requires(DP_Pixel15 p) { f(p); }
{
	for(auto tile : ActiveTileView(tlc)) {
		DP_Pixel15 *pixels = DP_transient_tile_pixels(tile.get());
		for(auto &pixel : std::span<DP_Pixel15>(pixels, DP_TILE_LENGTH))
			f(pixel);
	}
}

inline void thresholdAlpha(DP_TransientLayerContent *tlc, uint16_t value)
{
	forEachPixel(tlc, [value](DP_Pixel15 &pixel) {
		if(pixel.a > value) {
			DP_UPixel15 up = DP_pixel15_unpremultiply(pixel);
			pixel.b = up.b;
			pixel.g = up.g;
			pixel.r = up.r;
			pixel.a = DP_BIT15;
		} else
			pixel = {0, 0, 0, 0};
	});
}

struct BGRA8OffsetImage {
	BGRA8OffsetImage() = default;

	BGRA8OffsetImage(DP_UPixel8 *pixels, int x, int y, int width, int height)
		: pixels(pixels)
		, x(x)
		, y(y)
		, width(width)
		, height(height)
	{
	}
	BGRA8OffsetImage(BGRA8OffsetImage &&other) noexcept
		: pixels(other.pixels)
		, x(other.x)
		, y(other.y)
		, width(other.width)
		, height(other.height)
	{
		other.pixels = nullptr;
	}
	~BGRA8OffsetImage() { DP_free(pixels); }

	BGRA8OffsetImage &operator=(BGRA8OffsetImage &&other) noexcept
	{
		if(&other == this)
			return *this;
		DP_free(pixels);
		pixels = nullptr;
		std::swap(pixels, other.pixels);
		x = other.x;
		y = other.y;
		width = other.width;
		height = other.height;
		return *this;
	}

	size_t sizeInPixels() const { return width * height; }
	size_t sizeInBytes() const { return sizeInPixels() * sizeof(DP_UPixel8); }

	void crop(DP_UPixel8 trimColor);
	std::vector<char> toPng() const;

	DP_UPixel8 *pixels = nullptr;
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
};

struct LayerRenderer {
	explicit LayerRenderer(drawdance::CanvasState cs);
	LayerRenderer(LayerRenderer &&) noexcept = default;
	LayerRenderer(const LayerRenderer &) = delete;
	LayerRenderer &operator=(LayerRenderer &&) noexcept = default;

	void renderLayer(Layer &layer, DP_TransientLayerContent *tlc);

	// Remove pixels in `layer` that have an alpha of > 0 in `mask`.
	void
	clipLayer(DP_TransientLayerContent *layer, DP_TransientLayerContent *mask);

	void renderLayerGroup(Layer &layer);

	void renderTopLevelLayer(Layer &layer);

	void clipPrevious(int upTo, DP_TransientLayerContent *mask);

	void render();

	BGRA8OffsetImage toPixels(
		DP_TransientLayerContent *tlc, bool crop,
		std::span<DP_UPixel8> palette) const;

	/// Keep the layer objects associated with this CanvasState live.
	drawdance::CanvasState m_associatedCanvasState;
	int width;
	int height;
	std::vector<Layer> m_rootLayers;
	Layer m_fullImage{ParsedTitle{false, true}, nullptr, nullptr};
};

#endif // HEADLESS_CLIENT_LAYER_RENDERER_H