"""Generate AutoCoder application icon (PNG + ICO)."""
import struct, zlib, io, os, sys

def create_png(width, height, pixels):
    """Create a PNG from RGBA pixel data (list of (r,g,b,a) tuples, row-major)."""
    # Filter bytes per row: 0 (None) at start of each row
    raw = b''
    for y in range(height):
        raw += b'\x00'  # filter byte: None
        row_start = y * width
        for x in range(width):
            r, g, b, a = pixels[row_start + x]
            raw += struct.pack('BBBB', r, g, b, a)

    def make_chunk(ctype, data):
        chunk = ctype + data
        crc = struct.pack('>I', zlib.crc32(chunk) & 0xffffffff)
        return struct.pack('>I', len(data)) + chunk + crc

    sig = b'\x89PNG\r\n\x1a\n'
    ihdr = struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)  # 8-bit RGBA
    idat = zlib.compress(raw)
    iend = b''

    return sig + make_chunk(b'IHDR', ihdr) + make_chunk(b'IDAT', idat) + make_chunk(b'IEND', iend)


def make_ico(png_data_list, sizes):
    """Create an ICO file from a list of PNG data for given sizes."""
    count = len(png_data_list)
    header = struct.pack('<HHH', 0, 1, count)
    offset = 6 + count * 16
    dir_entries = b''
    for i, (png, w, h) in enumerate(zip(png_data_list, sizes, sizes)):
        bpp = 32
        w_entry = 0 if w == 256 else w
        h_entry = 0 if h == 256 else h
        dir_entries += struct.pack('<BBBBHHII',
            w_entry, h_entry, 0, 0, 1, bpp, len(png), offset)
        offset += len(png)
    result = header + dir_entries
    for png in png_data_list:
        result += png
    return result


def linear_gradient(width, height, x1, y1, x2, y2, c1, c2, colors):
    """Interpolate color based on position projected onto gradient vector."""
    dx = x2 - x1
    dy = y2 - y1
    dot_max = dx*dx + dy*dy
    if dot_max == 0:
        dot_max = 1
    result = []
    for y in range(height):
        for x in range(width):
            px = x - x1
            py = y - y1
            t = (px*dx + py*dy) / dot_max
            t = max(0.0, min(1.0, t))
            # Map t through color stops
            num = len(colors) - 1
            scaled = t * num
            idx = int(scaled)
            frac = scaled - idx
            if idx >= num:
                idx = num - 1
                frac = 1.0
            ca = colors[idx]
            cb = colors[idx + 1]
            r = int(ca[0] + (cb[0] - ca[0]) * frac)
            g = int(ca[1] + (cb[1] - ca[1]) * frac)
            b = int(ca[2] + (cb[2] - ca[2]) * frac)
            result.append((r, g, b, 255))
    return result


def build_autocoder_icon(size):
    """Build a 256x256 AutoCoder icon. Returns RGBA pixel list."""
    w, h = size, size
    cx, cy = w // 2, h // 2
    r = w * 0.42  # main circle radius

    # Determine base colours
    dark_bg = (18, 20, 30)      # very dark blue-black
    accent1 = (59, 130, 246)    # blue-500
    accent2 = (139, 92, 246)    # purple-500
    accent3 = (236, 72, 153)    # pink-500
    white = (255, 255, 255)

    # Background: dark gradient from top-left to bottom-right
    bg_colors = [(18, 20, 30), (24, 28, 42), (30, 35, 55)]
    bg = linear_gradient(w, h, 0, 0, w, h, 0.0, 1.0,
                         [(18, 20, 30), (24, 28, 42), (30, 35, 55)])

    # We'll draw the logo on a transparent layer then composite onto bg
    # Actually let's just compute final pixel directly
    pixels = list(bg)

    def set_pixel(x, y, r_, g_, b_, a_=255):
        if 0 <= x < w and 0 <= y < h:
            idx = y * w + x
            if a_ == 255:
                pixels[idx] = (r_, g_, b_, 255)
            else:
                # Alpha blend over existing
                po = pixels[idx]
                f = a_ / 255.0
                pixels[idx] = (
                    int(po[0] * (1 - f) + r_ * f),
                    int(po[1] * (1 - f) + g_ * f),
                    int(po[2] * (1 - f) + b_ * f),
                    255
                )

    def aa_circle(cx_, cy_, rad, r_, g_, b_, aa=1.0):
        """Draw a filled anti-aliased circle."""
        x0 = int(cx_ - rad - 2)
        x1 = int(cx_ + rad + 2)
        y0 = int(cy_ - rad - 2)
        y1 = int(cy_ + rad + 2)
        r2 = rad * rad
        for y in range(max(0, y0), min(h, y1)):
            for x in range(max(0, x0), min(w, x1)):
                dx = (x + 0.5) - cx_
                dy = (y + 0.5) - cy_
                d2 = dx*dx + dy*dy
                if d2 <= r2:
                    # Fully inside
                    set_pixel(x, y, r_, g_, b_)
                else:
                    # Anti-alias edge
                    dist = (dx*dx + dy*dy) ** 0.5
                    alpha = max(0.0, min(1.0, (rad + 0.7 - dist) * 1.5))
                    if alpha > 0.01:
                        po = pixels[y * w + x]
                        f = alpha
                        set_pixel(x, y,
                                  int(po[0] * (1 - f) + r_ * f),
                                  int(po[1] * (1 - f) + g_ * f),
                                  int(po[2] * (1 - f) + b_ * f))

    def draw_rounded_rect(x0, y0, x1, y1, corner_r, r_, g_, b_):
        """Draw a filled anti-aliased rounded rectangle."""
        for y in range(max(0, y0), min(h, y1 + 1)):
            for x in range(max(0, x0), min(w, x1 + 1)):
                # Determine distance to nearest edge
                dx = 0
                if x < x0 + corner_r:
                    dx = x0 + corner_r - (x + 0.5)
                elif x > x1 - corner_r:
                    dx = (x + 0.5) - (x1 - corner_r)
                dy = 0
                if y < y0 + corner_r:
                    dy = y0 + corner_r - (y + 0.5)
                elif y > y1 - corner_r:
                    dy = (y + 0.5) - (y1 - corner_r)
                if dx > 0 and dy > 0:
                    dist = (dx*dx + dy*dy) ** 0.5
                    if dist <= corner_r:
                        alpha = min(1.0, (corner_r + 0.5 - dist) * 1.5)
                        if alpha < 0.01:
                            continue
                        po = pixels[y * w + x]
                        f = alpha
                        set_pixel(x, y,
                                  int(po[0] * (1 - f) + r_ * f),
                                  int(po[1] * (1 - f) + g_ * f),
                                  int(po[2] * (1 - f) + b_ * f))
                    continue
                # Inside
                set_pixel(x, y, r_, g_, b_)

    # Glow effect behind the main circle
    glow_colors = [(59, 130, 246, 30), (139, 92, 246, 20), (236, 72, 153, 10)]
    for gx, gy, grad_r, gc in [
            (cx - r*0.2, cy - r*0.2, r * 1.1, (59, 130, 246, 30)),
            (cx + r*0.1, cy + r*0.1, r * 0.9, (139, 92, 246, 20)),
            (cx, cy, r * 1.3, (236, 72, 153, 10))]:
        x0 = int(gx - grad_r)
        x1 = int(gx + grad_r)
        y0 = int(gy - grad_r)
        y1 = int(gy + grad_r)
        for y in range(max(0, y0), min(h, y1)):
            for x in range(max(0, x0), min(w, x1)):
                dist = ((x + 0.5 - gx)**2 + (y + 0.5 - gy)**2) ** 0.5
                alpha = max(0.0, 1.0 - dist / grad_r)
                alpha *= gc[3] / 255.0
                if alpha > 0.01:
                    po = pixels[y * w + x]
                    f = alpha
                    set_pixel(x, y,
                              int(po[0] * (1 - f) + gc[0] * f),
                              int(po[1] * (1 - f) + gc[1] * f),
                              int(po[2] * (1 - f) + gc[2] * f))

    # Main circle (the "O" / background shape)
    # Use a gradient: top-left -> bottom-right from blue to purple
    for y in range(h):
        for x in range(w):
            dx = (x + 0.5) - cx
            dy = (y + 0.5) - cy
            dist = (dx*dx + dy*dy) ** 0.5
            if dist <= r:
                # Gradient across the circle
                t = ((x / w) + (y / h)) * 0.5
                t = max(0.0, min(1.0, t))
                r_val = int(accent1[0] + (accent2[0] - accent1[0]) * t)
                g_val = int(accent1[1] + (accent2[1] - accent1[1]) * t)
                b_val = int(accent1[2] + (accent2[2] - accent1[2]) * t)
                set_pixel(x, y, r_val, g_val, b_val)
    # Anti-alias circle edge
    aa_circle(cx, cy, r, 0, 0, 0, 0)  # dummy, already filled

    # Inner darker circle (cutout for depth)
    inner_r = r * 0.72
    for y in range(h):
        for x in range(w):
            dx = (x + 0.5) - cx
            dy = (y + 0.5) - cy
            dist = (dx*dx + dy*dy) ** 0.5
            if dist <= inner_r:
                # Slightly lighter than bg
                po = pixels[y * w + x]
                r_val = int(po[0] * 0.7 + 30 * 0.3)
                g_val = int(po[1] * 0.7 + 35 * 0.3)
                b_val = int(po[2] * 0.7 + 50 * 0.3)
                set_pixel(x, y, r_val, g_val, b_val)

    # Draw a stylized "A" in the center
    # Made of thick strokes
    def draw_line(x0, y0, x1, y1, r_, g_, b_, thickness):
        """Bresenham-esque thick line with anti-aliasing."""
        dx = x1 - x0
        dy = y1 - y0
        length = (dx*dx + dy*dy) ** 0.5
        if length < 0.5:
            return
        dx /= length
        dy /= length
        # Perpendicular unit vector
        px = -dy
        py = dx
        half_t = thickness / 2.0

        y_min = max(0, int(min(y0, y1) - half_t - 1))
        y_max = min(h - 1, int(max(y0, y1) + half_t + 1))
        x_min = max(0, int(min(x0, x1) - half_t - 1))
        x_max = min(w - 1, int(max(x0, x1) + half_t + 1))

        for y in range(y_min, y_max + 1):
            for x in range(x_min, x_max + 1):
                # Project onto line
                vx = x + 0.5 - x0
                vy = y + 0.5 - y0
                t_proj = vx * dx + vy * dy
                t_proj = max(0.0, min(length, t_proj))
                closest_x = x0 + t_proj * dx
                closest_y = y0 + t_proj * dy
                dist = ((x + 0.5 - closest_x)**2 + (y + 0.5 - closest_y)**2) ** 0.5
                if dist <= half_t:
                    alpha = min(1.0, (half_t + 0.5 - dist) * 2.0)
                    if alpha > 0.01:
                        po = pixels[y * w + x]
                        f = alpha
                        set_pixel(x, y,
                                  int(po[0] * (1 - f) + r_ * f),
                                  int(po[1] * (1 - f) + g_ * f),
                                  int(po[2] * (1 - f) + b_ * f))

    # "A" letter: left stroke, right stroke, crossbar
    a_cx = cx
    a_cy = cy + 4  # slightly down to center visually
    a_size = r * 0.48
    a_thick = max(8, int(r * 0.13))

    # Left stroke: from top to bottom-left
    x0 = a_cx
    y0 = a_cy - a_size
    x1 = a_cx - a_size * 0.75
    y1 = a_cy + a_size * 0.6
    draw_line(x0, y0, x1, y1, 255, 255, 255, a_thick)

    # Right stroke: from top to bottom-right
    x0 = a_cx
    y0 = a_cy - a_size
    x1 = a_cx + a_size * 0.75
    y1 = a_cy + a_size * 0.6
    draw_line(x0, y0, x1, y1, 255, 255, 255, a_thick)

    # Crossbar
    cross_y = a_cy + a_size * 0.15
    draw_line(a_cx - a_size * 0.45, cross_y,
              a_cx + a_size * 0.45, cross_y,
              255, 255, 255, a_thick * 0.7)

    # Code brackets < > beside the A
    bracket_r = a_size * 0.25
    bracket_thick = max(5, int(r * 0.08))
    bracket_color = (168, 185, 212)  # light blue-grey

    # Left bracket <
    bx = a_cx - a_size * 1.05
    by = a_cy
    # < top stroke
    draw_line(bx, by - bracket_r, bx + bracket_r * 0.9, by,
              bracket_color[0], bracket_color[1], bracket_color[2], bracket_thick)
    # < bottom stroke
    draw_line(bx, by + bracket_r, bx + bracket_r * 0.9, by,
              bracket_color[0], bracket_color[1], bracket_color[2], bracket_thick)

    # Right bracket >
    bx = a_cx + a_size * 1.05
    draw_line(bx, by - bracket_r, bx - bracket_r * 0.9, by,
              bracket_color[0], bracket_color[1], bracket_color[2], bracket_thick)
    draw_line(bx, by + bracket_r, bx - bracket_r * 0.9, by,
              bracket_color[0], bracket_color[1], bracket_color[2], bracket_thick)

    # Add some small decorative dots (like code comments)
    dot_r = max(2, int(r * 0.02))
    for dot in [(cx + r * 0.65, cy - r * 0.55),
                (cx - r * 0.4, cy + r * 0.7),
                (cx + r * 0.3, cy + r * 0.75)]:
        aa_circle(dot[0], dot[1], dot_r, 168, 185, 212, 0.7)

    return pixels


def main():
    sizes = [256, 64, 48, 32, 24, 16]
    out_dir = os.path.join(os.path.dirname(__file__))
    
    png_data_list = []
    for size in sizes:
        print(f"Rendering {size}x{size}...")
        pixels = build_autocoder_icon(size)
        png = create_png(size, size, pixels)
        fn = os.path.join(out_dir, f"autocoder_{size}.png")
        with open(fn, 'wb') as f:
            f.write(png)
        print(f"  -> {fn} ({len(png)} bytes)")
        png_data_list.append(png)
    
    # Create ICO
    ico_data = make_ico(png_data_list, sizes)
    ico_fn = os.path.join(out_dir, "autocoder.ico")
    with open(ico_fn, 'wb') as f:
        f.write(ico_data)
    print(f"ICO -> {ico_fn} ({len(ico_data)} bytes)")
    
    # Also save the main 256 PNG as the primary icon asset
    src_256 = os.path.join(out_dir, "autocoder_256.png")
    dst_big = os.path.join(out_dir, "..", "autocoder.png")
    with open(src_256, 'rb') as fin:
        with open(dst_big, 'wb') as fout:
            fout.write(fin.read())
    print(f"Main PNG -> {dst_big}")


if __name__ == '__main__':
    main()
