#ifndef HEADLESS_CLIENT_ITERATORS_H
#define HEADLESS_CLIENT_ITERATORS_H

#include <ranges>
extern "C" {
#include <dpengine/layer_content.h>
#include <dpengine/tile.h>
}

/// View of non-blank tiles.
class ActiveTileView : public std::ranges::view_interface<ActiveTileView> {
public:
	class iterator;

	class ActiveTile {
	public:
		DP_TransientTile *get() const { return m_tt; }
		int x() const { return m_x; }
		int y() const { return m_y; }

	private:
		friend class iterator;

		explicit ActiveTile(DP_TransientTile *tt, int x, int y)
			: m_tt(tt)
			, m_x(x)
			, m_y(y)
		{
		}

		int m_x, m_y;
		DP_TransientTile *m_tt;
	};

	class iterator {
	public:
		using value_type = ActiveTile;
		using reference = value_type &;
		using pointer = value_type *;
		using iterator_category = std::forward_iterator_tag;

		value_type operator*() const
		{
			int y = m_currentTile / m_view.m_width;
			int x = m_currentTile % m_view.m_width;
			return ActiveTile{currentTile(), x, y};
		}

		iterator &operator++()
		{
			++m_currentTile;
			while(m_currentTile < m_view.m_width * m_view.m_height) {
				if(currentTile())
					break;
				++m_currentTile;
			}
			return *this;
		}
		void operator++(int) { ++(*this); }

		friend bool operator==(const iterator &a, const iterator &b)
		{
			return a.m_currentTile == b.m_currentTile &&
				   a.m_view.m_tlc == b.m_view.m_tlc;
		}

	private:
		friend class ActiveTileView;
		DP_TransientTile *currentTile() const
		{
			int y = m_currentTile / m_view.m_width;
			int x = m_currentTile % m_view.m_width;
			return DP_transient_layer_content_tile_at_noinc(m_view.m_tlc, x, y);
		}

		explicit iterator(ActiveTileView &view)
			: m_view(view)
		{
			if(!currentTile())
				++*this;
		}
		iterator(ActiveTileView &view, int index)
			: m_view(view)
			, m_currentTile(index)
		{
		}

		int m_currentTile = 0;
		ActiveTileView &m_view;
	};

	explicit ActiveTileView(DP_TransientLayerContent *tlc)
		: m_tlc(tlc)
	{
		m_width = DP_tile_count_round(DP_transient_layer_content_width(tlc));
		m_height = DP_tile_count_round(DP_transient_layer_content_height(tlc));
	}

	iterator begin() { return iterator{*this}; }
	iterator end() { return {*this, m_width * m_height}; }

private:
	friend class ActiveTileView::iterator;
	friend bool operator==(const iterator &a, const iterator &b);

	int m_width;
	int m_height;
	DP_TransientLayerContent *m_tlc;
};

#endif // HEADLESS_CLIENT_ITERATORS_H
