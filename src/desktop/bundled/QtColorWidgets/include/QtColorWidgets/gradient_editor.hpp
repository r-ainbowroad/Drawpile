/*
 * SPDX-FileCopyrightText: 2013-2020 Mattia Basaglia
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef GRADIENT_EDITOR_HPP
#define GRADIENT_EDITOR_HPP

#include "colorwidgets_global.hpp"

#include <QWidget>
#include <QGradient>

namespace color_widgets {

class ColorDialog;

/**
 * \brief A slider that moves on top of a gradient
 */
class QCP_EXPORT GradientEditor : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(QBrush background READ background WRITE setBackground NOTIFY backgroundChanged)
    Q_PROPERTY(QGradientStops stops READ stops WRITE setStops NOTIFY stopsChanged)
    Q_PROPERTY(QLinearGradient gradient READ gradient WRITE setGradient)
    Q_PROPERTY(Qt::Orientation orientation READ orientation WRITE setOrientation)
    Q_PROPERTY(int selectedStop READ selectedStop WRITE setSelectedStop NOTIFY selectedStopChanged)
    Q_PROPERTY(QColor selectedColor READ selectedColor WRITE setSelectedColor)

public:
    explicit GradientEditor(QWidget *parent = nullptr);
    explicit GradientEditor(Qt::Orientation orientation, QWidget *parent = nullptr);
    ~GradientEditor();

    QSize sizeHint() const override;

    /// Get the background, it's visible for transparent gradient stops
    QBrush background() const;
    /// Set the background, it's visible for transparent gradient stops
    void setBackground(const QBrush &bg);

    /// Get the colors that make up the gradient
    QGradientStops stops() const;
    /// Set the colors that make up the gradient
    void setStops(const QGradientStops &colors);

    /// Get the gradient
    QLinearGradient gradient() const;
    /// Set the gradient
    void setGradient(const QLinearGradient &gradient);

    Qt::Orientation orientation() const;

    /**
     * \brief Dialog shown when double clicking a stop
     */
    ColorDialog* dialog() const;

    /**
     * \brief Index of the currently selected gradient stop (or -1 if there is no selection)
     */
    int selectedStop() const;

    /**
     * \brief Color of the selected stop
     */
    QColor selectedColor() const;

public Q_SLOTS:
    void setOrientation(Qt::Orientation);
    void setSelectedStop(int stop);
    void setSelectedColor(const QColor& color);
    void addStop();
    void removeStop();

Q_SIGNALS:
    void backgroundChanged(const QBrush&);
    void stopsChanged(const QGradientStops&);
    void selectedStopChanged(int);

protected:
    void paintEvent(QPaintEvent *ev) override;

    void mousePressEvent(QMouseEvent *ev) override;
    void mouseMoveEvent(QMouseEvent *ev) override;
    void mouseReleaseEvent(QMouseEvent *ev) override;
    void leaveEvent(QEvent * event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent* event) override;

private Q_SLOTS:
    void dialogUpdate(const QColor& c);

private:
    class Private;
    Private * const p;
};

} // namespace color_widgets

#endif // GRADIENT_EDITOR_HPP

