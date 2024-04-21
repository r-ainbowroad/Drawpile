#include "LayerRenderer.h"
#include "Palettize.h"
extern "C" {
#include <dpcommon/geom.h>
#include <dpcommon/output.h>
#include <dpengine/image.h>
#include <dpengine/layer_content.h>
#include <dpengine/tile.h>
#include <dpmsg/blend_mode.h>
}

ParsedTitle parse_layer_title(std::string_view title)
{
	ParsedTitle ret;

	enum { Attributes, Name, Priority, Done } state = Attributes;

	const char *name_start = nullptr;
	int name_len = 0;
	for(const char &c : title) {
		switch(state) {
		case Attributes:
			switch(c) {
			case '-':
				ret.is_hidden = true;
				break;
			case '>':
				ret.is_exported = true;
				break;
			case '#':
				ret.is_meta = true;
				break;
			case '^':
				ret.is_completed = true;
				break;
			case '%':
				ret.is_community = true;
				break;
			case ' ':
				break;
			default:
				name_start = &c;
				++name_len;
				state = Name;
			}
			break;
		case Name:
			if(c == '[') {
				state = Priority;
			} else {
				++name_len;
			}
			break;
		case Priority:
			ret.priority = c - '0';
			state = Done;
			break;
		default:
			break;
		}
	}
	if(ret.name.empty() && name_start)
		ret.name = std::string_view(name_start, name_len);
	auto drop_back = ret.name.find_last_not_of(' ');
	if(drop_back != -1)
		ret.name.remove_suffix((ret.name.size() - drop_back) - 1);
	return ret;
}

std::vector<Layer>
Layer::fromLayerList(DP_LayerList *ll, DP_LayerPropsList *lpl)
{
	std::vector<Layer> ret;

	int count = DP_layer_list_count(ll);
	for(int i = 0; i < count; ++i) {
		DP_LayerListEntry *lle = DP_layer_list_at_noinc(ll, i);
		DP_LayerProps *lp = DP_layer_props_list_at_noinc(lpl, i);

		std::size_t len;
		const char *title_str = DP_layer_props_title(lp, &len);
		std::string_view title(title_str, len);
		ParsedTitle pt = parse_layer_title(title);

		if(pt.shouldSkip())
			continue;

		ret.emplace_back(pt, lle, lp);
	}

	return ret;
}

void BGRA8OffsetImage::crop(DP_UPixel8 trimColor)
{
	int x1 = width + 1, x2 = -1, y1 = height + 1, y2 = -1;
	for(int yp = 0; yp < height; ++yp)
		for(int xp = 0; xp < width; ++xp)
			if(pixels[yp * width + xp] != trimColor) {
				x1 = std::min(x1, xp);
				x2 = std::max(x2, xp);
				y1 = std::min(y1, yp);
				y2 = std::max(y2, yp);
			}
	if(x2 == -1) {
		// Couldn't find any non-trimmed pixels. Delete the entire image.
		DP_free(pixels);
		pixels = nullptr;
		x = y = width = height = 0;
		return;
	}
	int newWidth = (x2 - x1) + 1;
	int newHeight = (y2 - y1) + 1;
	DP_UPixel8 *newPixels =
		(DP_UPixel8 *)DP_malloc(newWidth * newHeight * sizeof(DP_UPixel8));
	for(int yp = 0; yp < newHeight; ++yp)
		for(int xp = 0; xp < newWidth; ++xp)
			newPixels[yp * newWidth + xp] =
				pixels[(yp + y1) * width + (xp + x1)];
	DP_free(pixels);
	pixels = newPixels;
	x = x1;
	y = y1;
	width = newWidth;
	height = newHeight;
}

void BGRA8OffsetImage::copyFrom(
	const BGRA8OffsetImage &other, int offsetX, int offsetY)
{
	if(other.sizeInPixels() == 0 || !other.pixels)
		return;
	if(offsetX <= -other.width || offsetX >= width ||
	   offsetY <= -other.height || offsetY >= height)
		return;
	int startX = std::clamp(offsetX, 0, width);
	int startY = std::clamp(offsetY, 0, height);
	int endX = std::clamp(offsetX + other.width, 0, width);
	int endY = std::clamp(offsetY + other.height, 0, height);

	// We now know there is at least one pixel of overlap and have start and end
	// bounds.
	for(int yp = startY; yp < endY; ++yp)
		for(int xp = startX; xp < endX; ++xp)
			pixels[yp * width + xp] =
				other.pixels[(yp - offsetY) * other.width + (xp - offsetX)];
}

std::vector<char> BGRA8OffsetImage::toPng() const
{
	void **buffer;
	size_t *size;
	DP_Image *img = DP_image_new(width, height);
	DP_Output *memOut = DP_mem_output_new(0, true, &buffer, &size);
	memcpy(DP_image_pixels(img), pixels, sizeInBytes());
	DP_image_write_png(img, memOut);

	std::vector<char> ret(
		static_cast<char *>(*buffer), static_cast<char *>(*buffer) + *size);

	DP_output_free(memOut);
	DP_image_free(img);
	return ret;
}

LayerRenderer::LayerRenderer(drawdance::CanvasState cs)
	: m_associatedCanvasState(std::move(cs))
{
	width = m_associatedCanvasState.width();
	height = m_associatedCanvasState.height();
	DP_LayerList *ll =
		DP_canvas_state_layers_noinc(m_associatedCanvasState.get());
	DP_LayerPropsList *lpl =
		DP_canvas_state_layer_props_noinc(m_associatedCanvasState.get());
	m_rootLayers = Layer::fromLayerList(ll, lpl);
}

void LayerRenderer::renderLayer(Layer &layer, DP_TransientLayerContent *tlc)
{
	DP_transient_layer_content_merge(
		tlc, 0, layer.m_lc, layer.opacity(), layer.blendMode(), false);
}

// Remove pixels in `layer` that have an alpha of > 0 in `mask`.
void LayerRenderer::clipLayer(
	DP_TransientLayerContent *layer, DP_TransientLayerContent *mask)
{
	if(!layer || !mask)
		return;
	for(auto tile : ActiveTileView(mask)) {
		DP_TransientTile *tt =
			DP_transient_layer_content_tile_at_noinc(layer, tile.x(), tile.y());
		if(!tt)
			continue;
		DP_Pixel15 *mask_pixels = DP_transient_tile_pixels(tile.get());
		DP_Pixel15 *layer_pixels = DP_transient_tile_pixels(tt);
		for(int i = 0; i < DP_TILE_LENGTH; ++i) {
			if(mask_pixels[i].a > DP_BIT15 / 2)
				layer_pixels[i] = DP_pixel15_zero();
		}
	}
}

void LayerRenderer::renderLayerGroup(Layer &layer)
{
	for(auto &child : layer.m_children) {
		if(child.isGroup()) {
			renderLayerGroup(child);
			if(child.m_renderedLayer)
				DP_transient_layer_content_merge(
					layer.getOrCreateRenderedTLC(width, height), 0,
					child.m_renderedLayer, child.opacity(), child.blendMode(),
					false);
			if(child.m_exclusionMask)
				DP_transient_layer_content_merge(
					layer.getOrCreateExclusionTLC(width, height), 0,
					child.m_exclusionMask, child.opacity(), child.blendMode(),
					false);
		} else {
			if(child.parsedTitle().is_exported)
				renderLayer(child, layer.getOrCreateRenderedTLC(width, height));
			else
				renderLayer(
					child, layer.getOrCreateExclusionTLC(width, height));
		}
		if(child.parsedTitle().is_exported)
			clipLayer(layer.m_exclusionMask, layer.m_renderedLayer);
		else
			clipLayer(layer.m_renderedLayer, layer.m_exclusionMask);
	}
}

void LayerRenderer::renderTopLevelLayer(Layer &layer)
{
	if(layer.isGroup()) {
		renderLayerGroup(layer);
	} else {
		DP_TransientLayerContent *target =
			layer.m_title.is_exported
				? layer.getOrCreateRenderedTLC(width, height)
				: layer.getOrCreateExclusionTLC(width, height);
		renderLayer(layer, target);
	}
	if(layer.m_renderedLayer)
		thresholdAlpha(layer.m_renderedLayer, DP_BIT15 / 2);
	if(layer.m_exclusionMask)
		thresholdAlpha(layer.m_exclusionMask, DP_BIT15 / 2);
}

void LayerRenderer::clipPrevious(int upTo, DP_TransientLayerContent *mask)
{
	for(int i = 0; i <= upTo; ++i) {
		if(!m_rootLayers[i].m_renderedLayer)
			continue;
		clipLayer(m_rootLayers[i].m_renderedLayer, mask);
	}
}

void LayerRenderer::render()
{
	m_fullImage.getOrCreateRenderedTLC(width, height);
	m_fullImage.getOrCreateExclusionTLC(width, height);
	int i = 0;
	for(auto &layer : m_rootLayers) {
		renderTopLevelLayer(layer);
		if(layer.parsedTitle().is_exported) {
			if(layer.m_renderedLayer)
				DP_transient_layer_content_merge(
					m_fullImage.m_renderedLayer, 0, layer.m_renderedLayer,
					DP_BIT15, DP_BLEND_MODE_NORMAL, false);
			clipLayer(m_fullImage.m_exclusionMask, m_fullImage.m_renderedLayer);
			if(layer.isGroup() && layer.m_exclusionMask) {
				DP_transient_layer_content_merge(
					m_fullImage.m_exclusionMask, 0, layer.m_exclusionMask,
					DP_BIT15, DP_BLEND_MODE_NORMAL, false);
				clipLayer(
					m_fullImage.m_renderedLayer, m_fullImage.m_exclusionMask);
			}
			clipPrevious(i, m_fullImage.m_exclusionMask);
		} else {
			if(layer.m_exclusionMask)
				DP_transient_layer_content_merge(
					m_fullImage.m_exclusionMask, 0, layer.m_exclusionMask,
					DP_BIT15, DP_BLEND_MODE_NORMAL, false);
			clipLayer(m_fullImage.m_renderedLayer, m_fullImage.m_exclusionMask);
			clipPrevious(i, m_fullImage.m_exclusionMask);
		}
		++i;
	}
}

BGRA8OffsetImage LayerRenderer::toPixels(
	DP_TransientLayerContent *tlc, bool crop,
	std::span<DP_UPixel8> palette) const
{
	int x{}, y{}, w, h;
	DP_UPixel8 *pixels;
	if(crop)
		pixels =
			DP_layer_content_to_upixels8_cropped(tlc, false, &x, &y, &w, &h);
	else {
		w = width;
		h = height;
		pixels = DP_layer_content_to_upixels8(tlc, 0, 0, width, height);
	}
	if(!palette.empty())
		::palettize(std::span<DP_UPixel8>(pixels, w * h), palette);
	return {pixels, x, y, w, h};
}
