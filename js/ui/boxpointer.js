// -*- mode: js; js-indent-level: 4; indent-tabs-mode: nil -*-

const Clutter = imports.gi.Clutter;
const Lang = imports.lang;
const Meta = imports.gi.Meta;
const St = imports.gi.St;
const Shell = imports.gi.Shell;

const Main = imports.ui.main;
const Tweener = imports.ui.tweener;

const PopupAnimation = {
    NONE:  0,
    SLIDE: 1 << 0,
    FADE:  1 << 1,
    FULL:  ~0,
};

const POPUP_ANIMATION_TIME = 0.15;

/**
 * BoxPointer:
 * @side: side to draw the arrow on
 * @binProperties: Properties to set on contained bin
 *
 * An actor which displays a triangle "arrow" pointing to a given
 * side.  The .bin property is a container in which content can be
 * placed.  The arrow position may be controlled via
 * setArrowOrigin(). The arrow side might be temporarily flipped
 * depending on the box size and source position to keep the box
 * totally inside the monitor if possible.
 *
 */
const BoxPointer = new Lang.Class({
    Name: 'BoxPointer',

    _init: function(arrowSide, binProperties) {
        this._arrowSide = arrowSide;
        this._userArrowSide = arrowSide;
        this._arrowOrigin = 0;
        this.actor = new St.Bin({ x_fill: true,
                                  y_fill: true });
        this._container = new Shell.GenericContainer();
        this.actor.set_child(this._container);
        this._container.connect('get-preferred-width', Lang.bind(this, this._getPreferredWidth));
        this._container.connect('get-preferred-height', Lang.bind(this, this._getPreferredHeight));
        this._container.connect('allocate', Lang.bind(this, this._allocate));
        this.bin = new St.Bin(binProperties);
        this._container.add_actor(this.bin);
        this._border = new St.DrawingArea();
        this._border.connect('repaint', Lang.bind(this, this._drawBorder));
        this._container.add_actor(this._border);
        this.bin.raise(this._border);
        this._xOffset = 0;
        this._yOffset = 0;
        this._xPosition = 0;
        this._yPosition = 0;
        this._sourceAlignment = 0.5;
        this._capturedEventId = 0;
        this._muteInput();
    },

    _muteInput: function() {
        if (this._capturedEventId == 0)
            this._capturedEventId = this.actor.connect('captured-event',
                                                       function() { return true; });
    },

    _unmuteInput: function() {
        if (this._capturedEventId != 0) {
            this.actor.disconnect(this._capturedEventId);
            this._capturedEventId = 0;
        }
    },

    show: function(animate, onComplete) {
        let themeNode = this.actor.get_theme_node();
        let rise = themeNode.get_length('-arrow-rise');
        let animationTime = (animate & PopupAnimation.FULL) ? POPUP_ANIMATION_TIME : 0;

        if (animate & PopupAnimation.FADE)
            this.opacity = 0;
        else
            this.opacity = 255;

        this.actor.show();

        if (animate & PopupAnimation.SLIDE) {
            switch (this._arrowSide) {
                case St.Side.TOP:
                    this.yOffset = -rise;
                    break;
                case St.Side.BOTTOM:
                    this.yOffset = rise;
                    break;
                case St.Side.LEFT:
                    this.xOffset = -rise;
                    break;
                case St.Side.RIGHT:
                    this.xOffset = rise;
                    break;
            }
        }

        Tweener.addTween(this, { opacity: 255,
                                 xOffset: 0,
                                 yOffset: 0,
                                 transition: 'linear',
                                 onComplete: Lang.bind(this, function() {
                                     this._unmuteInput();
                                     if (onComplete)
                                         onComplete();
                                 }),
                                 time: animationTime });
    },

    hide: function(animate, onComplete) {
        let xOffset = 0;
        let yOffset = 0;
        let themeNode = this.actor.get_theme_node();
        let rise = themeNode.get_length('-arrow-rise');
        let fade = (animate & PopupAnimation.FADE);
        let animationTime = (animate & PopupAnimation.FULL) ? POPUP_ANIMATION_TIME : 0;

        if (animate & PopupAnimation.SLIDE) {
            switch (this._arrowSide) {
                case St.Side.TOP:
                    yOffset = rise;
                    break;
                case St.Side.BOTTOM:
                    yOffset = -rise;
                    break;
                case St.Side.LEFT:
                    xOffset = rise;
                    break;
                case St.Side.RIGHT:
                    xOffset = -rise;
                    break;
            }
        }

        this._muteInput();

        Tweener.removeTweens(this);
        Tweener.addTween(this, { opacity: fade ? 0 : 255,
                                 xOffset: xOffset,
                                 yOffset: yOffset,
                                 transition: 'linear',
                                 time: animationTime,
                                 onComplete: Lang.bind(this, function () {
                                     this.actor.hide();
                                     this.opacity = 0;
                                     this.xOffset = 0;
                                     this.yOffset = 0;
                                     if (onComplete)
                                         onComplete();
                                 })
                               });
    },

    _adjustAllocationForArrow: function(isWidth, alloc) {
        let themeNode = this.actor.get_theme_node();
        let borderWidth = themeNode.get_length('-arrow-border-width');
        alloc.min_size += borderWidth * 2;
        alloc.natural_size += borderWidth * 2;
        if ((!isWidth && (this._arrowSide == St.Side.TOP || this._arrowSide == St.Side.BOTTOM))
            || (isWidth && (this._arrowSide == St.Side.LEFT || this._arrowSide == St.Side.RIGHT))) {
            let rise = themeNode.get_length('-arrow-rise');
            alloc.min_size += rise;
            alloc.natural_size += rise;
        }
    },

    _getPreferredWidth: function(actor, forHeight, alloc) {
        let [minInternalSize, natInternalSize] = this.bin.get_preferred_width(forHeight);
        alloc.min_size = minInternalSize;
        alloc.natural_size = natInternalSize;
        this._adjustAllocationForArrow(true, alloc);
    },

    _getPreferredHeight: function(actor, forWidth, alloc) {
        let [minSize, naturalSize] = this.bin.get_preferred_height(forWidth);
        alloc.min_size = minSize;
        alloc.natural_size = naturalSize;
        this._adjustAllocationForArrow(false, alloc);
    },

    _allocate: function(actor, box, flags) {
        let themeNode = this.actor.get_theme_node();
        let borderWidth = themeNode.get_length('-arrow-border-width');
        let rise = themeNode.get_length('-arrow-rise');
        let childBox = new Clutter.ActorBox();
        let availWidth = box.x2 - box.x1;
        let availHeight = box.y2 - box.y1;

        childBox.x1 = 0;
        childBox.y1 = 0;
        childBox.x2 = availWidth;
        childBox.y2 = availHeight;
        this._border.allocate(childBox, flags);

        childBox.x1 = borderWidth;
        childBox.y1 = borderWidth;
        childBox.x2 = availWidth - borderWidth;
        childBox.y2 = availHeight - borderWidth;
        switch (this._arrowSide) {
            case St.Side.TOP:
                childBox.y1 += rise;
                break;
            case St.Side.BOTTOM:
                childBox.y2 -= rise;
                break;
            case St.Side.LEFT:
                childBox.x1 += rise;
                break;
            case St.Side.RIGHT:
                childBox.x2 -= rise;
                break;
        }
        this.bin.allocate(childBox, flags);

        if (this._sourceActor && this._sourceActor.mapped) {
            this._reposition(this._sourceActor, this._arrowAlignment);

            if (this._shouldFlip()) {
                switch (this._arrowSide) {
                case St.Side.TOP:
                    this._arrowSide = St.Side.BOTTOM;
                    break;
                case St.Side.BOTTOM:
                    this._arrowSide = St.Side.TOP;
                    break;
                case St.Side.LEFT:
                    this._arrowSide = St.Side.RIGHT;
                    break;
                case St.Side.RIGHT:
                    this._arrowSide = St.Side.LEFT;
                    break;
                }
                this._reposition(this._sourceActor, this._arrowAlignment);
            }
        }
    },

    _drawBorder: function(area) {
        let themeNode = this.actor.get_theme_node();

        let borderWidth = themeNode.get_length('-arrow-border-width');
        let base = themeNode.get_length('-arrow-base');
        let rise = themeNode.get_length('-arrow-rise');
        let borderRadius = themeNode.get_length('-arrow-border-radius');

        let halfBorder = borderWidth / 2;
        let halfBase = Math.floor(base/2);

        let backgroundColor = themeNode.get_color('-arrow-background-color');

        let [width, height] = area.get_surface_size();
        let [boxWidth, boxHeight] = [width, height];
        if (this._arrowSide == St.Side.TOP || this._arrowSide == St.Side.BOTTOM) {
            boxHeight -= rise;
        } else {
            boxWidth -= rise;
        }
        let cr = area.get_context();

        // Translate so that box goes from 0,0 to boxWidth,boxHeight,
        // with the arrow poking out of that
        if (this._arrowSide == St.Side.TOP) {
            cr.translate(0, rise);
        } else if (this._arrowSide == St.Side.LEFT) {
            cr.translate(rise, 0);
        }

        let [x1, y1] = [halfBorder, halfBorder];
        let [x2, y2] = [boxWidth - halfBorder, boxHeight - halfBorder];

        let skipTopLeft = false;
        let skipTopRight = false;
        let skipBottomLeft = false;
        let skipBottomRight = false;

        switch (this._arrowSide) {
        case St.Side.TOP:
            if (this._arrowOrigin == x1)
                skipTopLeft = true;
            else if (this._arrowOrigin == x2)
                skipTopRight = true;
            break;

        case St.Side.RIGHT:
            if (this._arrowOrigin == y1)
                skipTopRight = true;
            else if (this._arrowOrigin == y2)
                skipBottomRight = true;
            break;

        case St.Side.BOTTOM:
            if (this._arrowOrigin == x1)
                skipBottomLeft = true;
            else if (this._arrowOrigin == x2)
                skipBottomRight = true;
            break;

        case St.Side.LEFT:
            if (this._arrowOrigin == y1)
                skipTopLeft = true;
            else if (this._arrowOrigin == y2)
                skipBottomLeft = true;
            break;
        }

        cr.moveTo(x1 + borderRadius, y1);
        if (this._arrowSide == St.Side.TOP) {
            if (skipTopLeft) {
                cr.moveTo(x1, y2 - borderRadius);
                cr.lineTo(x1, y1 - rise);
                cr.lineTo(x1 + halfBase, y1);
            } else if (skipTopRight) {
                cr.lineTo(x2 - halfBase, y1);
                cr.lineTo(x2, y1 - rise);
                cr.lineTo(x2, y1 + borderRadius);
            } else {
                cr.lineTo(this._arrowOrigin - halfBase, y1);
                cr.lineTo(this._arrowOrigin, y1 - rise);
                cr.lineTo(this._arrowOrigin + halfBase, y1);
            }
        }

        if (!skipTopRight) {
            cr.lineTo(x2 - borderRadius, y1);
            cr.arc(x2 - borderRadius, y1 + borderRadius, borderRadius,
                   3*Math.PI/2, Math.PI*2);
        }

        if (this._arrowSide == St.Side.RIGHT) {
            if (skipTopRight) {
                cr.lineTo(x2 + rise, y1);
                cr.lineTo(x2 + rise, y1 + halfBase);
            } else if (skipBottomRight) {
                cr.lineTo(x2, y2 - halfBase);
                cr.lineTo(x2 + rise, y2);
                cr.lineTo(x2 - borderRadius, y2);
            } else {
                cr.lineTo(x2, this._arrowOrigin - halfBase);
                cr.lineTo(x2 + rise, this._arrowOrigin);
                cr.lineTo(x2, this._arrowOrigin + halfBase);
            }
        }

        if (!skipBottomRight) {
            cr.lineTo(x2, y2 - borderRadius);
            cr.arc(x2 - borderRadius, y2 - borderRadius, borderRadius,
                   0, Math.PI/2);
        }

        if (this._arrowSide == St.Side.BOTTOM) {
            if (skipBottomLeft) {
                cr.lineTo(x1 + halfBase, y2);
                cr.lineTo(x1, y2 + rise);
                cr.lineTo(x1, y2 - borderRadius);
            } else if (skipBottomRight) {
                cr.lineTo(x2, y2 + rise);
                cr.lineTo(x2 - halfBase, y2);
            } else {
                cr.lineTo(this._arrowOrigin + halfBase, y2);
                cr.lineTo(this._arrowOrigin, y2 + rise);
                cr.lineTo(this._arrowOrigin - halfBase, y2);
            }
        }

        if (!skipBottomLeft) {
            cr.lineTo(x1 + borderRadius, y2);
            cr.arc(x1 + borderRadius, y2 - borderRadius, borderRadius,
                   Math.PI/2, Math.PI);
        }

        if (this._arrowSide == St.Side.LEFT) {
            if (skipTopLeft) {
                cr.lineTo(x1, y1 + halfBase);
                cr.lineTo(x1 - rise, y1);
                cr.lineTo(x1 + borderRadius, y1);
            } else if (skipBottomLeft) {
                cr.lineTo(x1 - rise, y2)
                cr.lineTo(x1 - rise, y2 - halfBase);
            } else {
                cr.lineTo(x1, this._arrowOrigin + halfBase);
                cr.lineTo(x1 - rise, this._arrowOrigin);
                cr.lineTo(x1, this._arrowOrigin - halfBase);
            }
        }

        if (!skipTopLeft) {
            cr.lineTo(x1, y1 + borderRadius);
            cr.arc(x1 + borderRadius, y1 + borderRadius, borderRadius,
                   Math.PI, 3*Math.PI/2);
        }

        Clutter.cairo_set_source_color(cr, backgroundColor);
        cr.fillPreserve();

        if (borderWidth > 0) {
            let borderColor = themeNode.get_color('-arrow-border-color');
            Clutter.cairo_set_source_color(cr, borderColor);
            cr.setLineWidth(borderWidth);
            cr.stroke();
        }
    },

    setPosition: function(sourceActor, alignment) {
        this._arrowSide = this._userArrowSide;

        // We need to show it now to force an allocation,
        // so that we can query the correct size.
        this.actor.show();

        this._sourceActor = sourceActor;
        this._arrowAlignment = alignment;

        this._reposition(sourceActor, alignment);
    },

    setSourceAlignment: function(alignment) {
        this._sourceAlignment = alignment;

        if (!this._sourceActor)
            return;

        this.setPosition(this._sourceActor, this._arrowAlignment);
    },

    _reposition: function(sourceActor, alignment) {
        // Position correctly relative to the sourceActor
        let sourceNode = sourceActor.get_theme_node();
        let sourceContentBox = sourceNode.get_content_box(sourceActor.get_allocation_box());
        let sourceAllocation = Shell.util_get_transformed_allocation(sourceActor);
        let sourceCenterX = sourceAllocation.x1 + sourceContentBox.x1 + (sourceContentBox.x2 - sourceContentBox.x1) * this._sourceAlignment;
        let sourceCenterY = sourceAllocation.y1 + sourceContentBox.y1 + (sourceContentBox.y2 - sourceContentBox.y1) * this._sourceAlignment;
        let [minWidth, minHeight, natWidth, natHeight] = this.actor.get_preferred_size();

        // We also want to keep it onscreen, and separated from the
        // edge by the same distance as the main part of the box is
        // separated from its sourceActor
        let monitor = Main.layoutManager.findMonitorForActor(sourceActor);
        let themeNode = this.actor.get_theme_node();
        let borderWidth = themeNode.get_length('-arrow-border-width');
        let arrowBase = themeNode.get_length('-arrow-base');
        let borderRadius = themeNode.get_length('-arrow-border-radius');
        let margin = (4 * borderRadius + borderWidth + arrowBase);

        let gap = themeNode.get_length('-boxpointer-gap');
        let padding = themeNode.get_length('-arrow-rise');

        let resX, resY;

        switch (this._arrowSide) {
        case St.Side.TOP:
            resY = sourceAllocation.y2 + gap;
            break;
        case St.Side.BOTTOM:
            resY = sourceAllocation.y1 - natHeight - gap;
            break;
        case St.Side.LEFT:
            resX = sourceAllocation.x2 + gap;
            break;
        case St.Side.RIGHT:
            resX = sourceAllocation.x1 - natWidth - gap;
            break;
        }

        // Now align and position the pointing axis, making sure it fits on
        // screen. If the arrowOrigin is so close to the edge that the arrow
        // will not be isosceles, we try to compensate as follows:
        //   - We skip the rounded corner and settle for a right angled arrow
        //     as shown below. See _drawBorder for further details.
        //     |\_____
        //     |
        //     |
        //   - If the arrow was going to be acute angled, we move the position
        //     of the box to maintain the arrow's accuracy.

        let arrowOrigin;
        let halfBase = Math.floor(arrowBase/2);
        let halfBorder = borderWidth / 2;
        let halfMargin = margin / 2;
        let [x1, y1] = [halfBorder, halfBorder];
        let [x2, y2] = [natWidth - halfBorder, natHeight - halfBorder];

        switch (this._arrowSide) {
        case St.Side.TOP:
        case St.Side.BOTTOM:
            resX = sourceCenterX - (halfMargin + (natWidth - margin) * alignment);

            resX = Math.max(resX, monitor.x + padding);
            resX = Math.min(resX, monitor.x + monitor.width - (padding + natWidth));

            arrowOrigin = sourceCenterX - resX;
            if (arrowOrigin <= (x1 + (borderRadius + halfBase))) {
                if (arrowOrigin > x1)
                    resX += (arrowOrigin - x1);
                arrowOrigin = x1;
            } else if (arrowOrigin >= (x2 - (borderRadius + halfBase))) {
                if (arrowOrigin < x2)
                    resX -= (x2 - arrowOrigin);
                arrowOrigin = x2;
            }
            break;

        case St.Side.LEFT:
        case St.Side.RIGHT:
            resY = sourceCenterY - (halfMargin + (natHeight - margin) * alignment);

            resY = Math.max(resY, monitor.y + padding);
            resY = Math.min(resY, monitor.y + monitor.height - (padding + natHeight));

            arrowOrigin = sourceCenterY - resY;
            if (arrowOrigin <= (y1 + (borderRadius + halfBase))) {
                if (arrowOrigin > y1)
                    resY += (arrowOrigin - y1);
                arrowOrigin = y1;
            } else if (arrowOrigin >= (y2 - (borderRadius + halfBase))) {
                if (arrowOrigin < y2)
                    resX -= (y2 - arrowOrigin);
                arrowOrigin = y2;
            }
            break;
        }

        this.setArrowOrigin(arrowOrigin);

        let parent = this.actor.get_parent();
        let success, x, y;
        while (!success) {
            [success, x, y] = parent.transform_stage_point(resX, resY);
            parent = parent.get_parent();
        }

        this._xPosition = Math.floor(x);
        this._yPosition = Math.floor(y);
        this._shiftActor();
    },

    // @origin: Coordinate specifying middle of the arrow, along
    // the Y axis for St.Side.LEFT, St.Side.RIGHT from the top and X axis from
    // the left for St.Side.TOP and St.Side.BOTTOM.
    setArrowOrigin: function(origin) {
        if (this._arrowOrigin != origin) {
            this._arrowOrigin = origin;
            this._border.queue_repaint();
        }
    },

    _shiftActor : function() {
        // Since the position of the BoxPointer depends on the allocated size
        // of the BoxPointer and the position of the source actor, trying
        // to position the BoxPoiner via the x/y properties will result in
        // allocation loops and warnings. Instead we do the positioning via
        // the anchor point, which is independent of allocation, and leave
        // x == y == 0.
        this.actor.set_anchor_point(-(this._xPosition + this._xOffset),
                                    -(this._yPosition + this._yOffset));
    },

    _shouldFlip: function() {
        let sourceAllocation = Shell.util_get_transformed_allocation(this._sourceActor);
        let boxAllocation = Shell.util_get_transformed_allocation(this.actor);
        let boxWidth = boxAllocation.x2 - boxAllocation.x1;
        let boxHeight = boxAllocation.y2 - boxAllocation.y1;
        let monitor = Main.layoutManager.findMonitorForActor(this.actor);

        switch (this._arrowSide) {
        case St.Side.TOP:
            if (boxAllocation.y2 > monitor.y + monitor.height &&
                boxHeight < sourceAllocation.y1 - monitor.y)
                return true;
            break;
        case St.Side.BOTTOM:
            if (boxAllocation.y1 < monitor.y &&
                boxHeight < monitor.y + monitor.height - sourceAllocation.y2)
                return true;
            break;
        case St.Side.LEFT:
            if (boxAllocation.x2 > monitor.x + monitor.width &&
                boxWidth < sourceAllocation.x1 - monitor.x)
                return true;
            break;
        case St.Side.RIGHT:
            if (boxAllocation.x1 < monitor.x &&
                boxWidth < monitor.x + monitor.width - sourceAllocation.x2)
                return true;
            break;
        }

        return false;
    },

    set xOffset(offset) {
        this._xOffset = offset;
        this._shiftActor();
    },

    get xOffset() {
        return this._xOffset;
    },

    set yOffset(offset) {
        this._yOffset = offset;
        this._shiftActor();
    },

    get yOffset() {
        return this._yOffset;
    },

    set opacity(opacity) {
        this.actor.opacity = opacity;
    },

    get opacity() {
        return this.actor.opacity;
    }
});
