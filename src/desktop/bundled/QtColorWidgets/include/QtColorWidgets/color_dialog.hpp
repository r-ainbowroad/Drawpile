/*
 * SPDX-FileCopyrightText: 2013-2020 Mattia Basaglia
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

#ifndef COLOR_DIALOG_HPP
#define COLOR_DIALOG_HPP

#include "colorwidgets_global.hpp"
#include "color_preview.hpp"
#include "color_wheel.hpp"

#include <QDialog>

class QAbstractButton;

namespace color_widgets {

class QCP_EXPORT ColorDialog : public QDialog
{
    Q_OBJECT
    Q_ENUMS(ButtonMode)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged DESIGNABLE true)
    Q_PROPERTY(ColorWheel::ShapeEnum wheelShape READ wheelShape WRITE setWheelShape NOTIFY wheelShapeChanged)
    Q_PROPERTY(ColorWheel::ColorSpaceEnum colorSpace READ colorSpace WRITE setColorSpace NOTIFY colorSpaceChanged)
    Q_PROPERTY(bool wheelRotating READ wheelRotating WRITE setWheelRotating NOTIFY wheelRotatingChanged)
    Q_PROPERTY(bool wheelMirrored READ wheelMirrored WRITE setWheelMirrored NOTIFY wheelMirroredChanged)
    /**
     * \brief whether the color alpha channel can be edited.
     *
     * If alpha is disabled, the selected color's alpha will always be 255.
     */
    Q_PROPERTY(bool alphaEnabled READ alphaEnabled WRITE setAlphaEnabled NOTIFY alphaEnabledChanged)

public:
    enum ButtonMode {
        OkCancel,
        OkApplyCancel,
        Close
    };

    explicit ColorDialog(QWidget *parent = nullptr, Qt::WindowFlags f = {});

    ~ColorDialog();

    /**
     * Get currently selected color
     */
    QColor color() const;

    /**
     * Set the display mode for the color preview
     */
    void setPreviewDisplayMode(ColorPreview::DisplayMode mode);

    /**
     * Get the color preview diplay mode
     */
    ColorPreview::DisplayMode previewDisplayMode() const;

    bool alphaEnabled() const;

    /**
     * Select which dialog buttons to show
     *
     * There are three predefined modes:
     * OkCancel - this is useful when the dialog is modal and we just want to return a color
     * OkCancelApply - this is for non-modal dialogs
     * Close - for non-modal dialogs with direct color updates via colorChanged signal
     */
    void setButtonMode(ButtonMode mode);
    ButtonMode buttonMode() const;

    QSize sizeHint() const Q_DECL_OVERRIDE;

    ColorWheel::ShapeEnum wheelShape() const;
    ColorWheel::ColorSpaceEnum colorSpace() const;
    bool wheelRotating() const;
    bool wheelMirrored() const;

    int exec() Q_DECL_OVERRIDE;

public Q_SLOTS:

    /**
     * Change color
     */
    void setColor(const QColor &c);

	/**
     * Set the current color and show the dialog
     */
    void showColor(const QColor &oldcolor);

    void setWheelShape(ColorWheel::ShapeEnum shape);
    void setColorSpace(ColorWheel::ColorSpaceEnum space);
    void setWheelRotating(bool rotating);
    void setWheelMirrored(bool mirrored);

    /**
     * Set whether the color alpha channel can be edited.
     * If alpha is disabled, the selected color's alpha will always be 255.
     */
    void setAlphaEnabled(bool a);

Q_SIGNALS:
    /**
     * The current color was changed
     */
    void colorChanged(QColor);

    /**
     * The user selected the new color by pressing Ok/Apply
     */
    void colorSelected(QColor);

    void wheelShapeChanged(ColorWheel::ShapeEnum shape);
    void colorSpaceChanged(ColorWheel::ColorSpaceEnum space);
    void wheelRotatingChanged(bool rotating);
    void wheelMirroredChanged(bool mirrored);

    void alphaEnabledChanged(bool alphaEnabled);

private Q_SLOTS:
    /// Update all the Ui elements to match the selected color
    void setColorInternal(const QColor &color);
    /// Update from HSV sliders
    void set_hsv();
    /// Update from RGB sliders
    void set_rgb();
    /// Update from Alpha slider
    void set_alpha();

    void on_edit_hex_colorChanged(const QColor& color);
    void on_edit_hex_colorEditingFinished(const QColor& color);

    void on_buttonBox_clicked(QAbstractButton*);

protected:
    void dragEnterEvent(QDragEnterEvent *event) Q_DECL_OVERRIDE;
    void dropEvent(QDropEvent * event) Q_DECL_OVERRIDE;
    void mouseReleaseEvent(QMouseEvent *event) Q_DECL_OVERRIDE;
    void mouseMoveEvent(QMouseEvent *event) Q_DECL_OVERRIDE;
    void keyReleaseEvent(QKeyEvent *event) Q_DECL_OVERRIDE;

private:
    class Private;
    Private * const p;
};

} // namespace color_widgets

#endif // COLOR_DIALOG_HPP
