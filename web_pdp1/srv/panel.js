// The PDP-1 front panel in the browser: the console and the rack top (I/O) panel, drawn from the
// server's "panel" messages, see ../panel.go.
//
// The art and layout.json in panel/ are made by src/vpanel_pdp1/art/mkvpanelart.py from vpanel_pdp1's
// own art and element table, so this places every lamp, switch and key exactly where vpanel_pdp1 does.
// Coordinates there are logical pixels, the console 800 wide; the art is made at artScale times that.
//
// Mouse, as vpanel_pdp1: on a switch, left flips it, middle puts it up, right down;
// on a key, left presses it down and right lifts it up, both while the button is held.
// While the server says the hardware panel's driver runs, this only shows its switches.
//
// Lamps show on or off, as vpanel_pdp1 does: on when lit at least half the time since the server's last
// read, about every 33 ms, vpanel_pdp1's pace on a local display. Over ssh X forwarding, vpanel_pdp1's
// drawing slows it, so there it changes more slowly than this.
//
// 13-Nov-2026 Claude: initial version
// 14-Nov-2026 Claude: on/off lamps as vpanel_pdp1, glow as an option; nothing drawn while hidden
// 14-Nov-2026 Claude: base64 decoding for browsers without Uint8Array.fromBase64
// 15-Nov-2026 Claude: a key release always sent; the server's switches ignored briefly after a change
// 15-Nov-2026 Claude: glow mode removed, a lamp's on/off decided as each message arrives

// After a change, the server's switch words are ignored for this long: a message it read before the
// change was applied would otherwise show the old setting again for a moment.
const swHoldMs = 150;

// The lamps arrive base64 encoded, Go's encoding of a []byte.
// Uint8Array.fromBase64 is recent (Chrome 140, Firefox 133, Safari 18.2), older browsers decode with atob.
const fromBase64 = Uint8Array.fromBase64 ? (s) => Uint8Array.fromBase64(s) :
	(s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));

class FrontPanel {
	constructor(canvas, statusEl, sendFn) {
		this.canvas = canvas;
		this.ctx = canvas.getContext('2d');
		this.statusEl = statusEl;
		this.send = sendFn;

		this.layout = null;
		this.img = {};
		this.showIo = localStorage.getItem('panelShowIo') !== 'false';
		this.visible = true;                   // false while hidden: nothing is drawn

		this.present = false;
		this.mirror = false;
		this.sw = [0, 0, 0, 0];
		this.lit = new Uint8Array(180);        // each lamp on, 1, or off, 0, 10 rows of 18
		this.heldKey = null;                   // { word, mask } of the key the mouse holds
		this.swHoldUntil = 0;                  // performance.now() until which the server's sw is ignored
		this.dirty = true;

		canvas.addEventListener('mousedown', (e) => this.mouseDown(e));
		canvas.addEventListener('contextmenu', (e) => e.preventDefault());
		window.addEventListener('mouseup', () => this.releaseKey());
		canvas.addEventListener('mouseleave', () => this.releaseKey());
	}

	async load() {
		const names = ['pdp1_panel_2x', 'io_panel', 'lamp_on', 'lamp_off', 'switch_u', 'switch_d',
			'switch_l', 'switch_r', 'key_n', 'key_d', 'key_u'];
		const resp = await fetch('panel/layout.json');
		this.layout = await resp.json();
		await Promise.all(names.map((n) => new Promise((resolve, reject) => {
			const im = new Image();
			im.onload = () => resolve();
			im.onerror = reject;
			im.src = `panel/${n}.png`;
			this.img[n] = im;
		})));

		// Each lamp's index into the 180 brightness bytes: its row, and the column of its one bit.
		const index = (l) => l.row * 18 + Math.round(Math.log2(l.mask));
		for (const l of this.layout.lamps) l.index = index(l);
		for (const l of this.layout.iolamps) l.index = index(l);

		this.resize();
		requestAnimationFrame(() => this.frame());
	}

	setShowIo(on) {
		this.showIo = on;
		localStorage.setItem('panelShowIo', on);
		this.resize();
	}

	setVisible(on) {
		this.visible = on;
		this.dirty = true;
	}

	// Canvas pixels are the art's, artScale times logical, so it's sharp at any displayed size.
	resize() {
		const L = this.layout;
		if (!L) return;                        // the art hasn't loaded
		this.ioOff = this.showIo ? L.ioH : 0;
		this.canvas.width = L.consoleW * L.artScale;
		this.canvas.height = (L.consoleH + this.ioOff) * L.artScale;
		this.dirty = true;
	}

	// A "panel" message from the server.
	// Its lamps are a brightness, 0-255, the share of the cycles since the server's last read each was lit.
	// A lamp is on when lit at least half of them, PWM_ON_THRESHOLD in vpanel_pdp1; the server rounds down,
	// so exactly half arrives as 127.
	update(msg) {
		this.present = !!msg.present;
		this.mirror = !!msg.mirror;
		if (msg.sw && performance.now() >= this.swHoldUntil) this.sw = msg.sw.slice(0, 4);
		if (msg.lamps) this.lit = fromBase64(msg.lamps).map((b) => b >= 127 ? 1 : 0);
		else if (!this.present) this.lit = new Uint8Array(180);
		this.statusEl.textContent = !this.present ? 'emulator not running' :
			(this.mirror ? 'mirroring hardware panel' : '');
		this.dirty = true;
	}

	// Draws, at most once a display frame, when something changed and the panel is shown.
	frame() {
		if (this.dirty && this.visible && this.layout) {
			this.draw();
			this.dirty = false;
		}
		requestAnimationFrame(() => this.frame());
	}

	draw() {
		const L = this.layout, ctx = this.ctx, im = this.img;
		ctx.setTransform(L.artScale, 0, 0, L.artScale, 0, 0);

		if (this.showIo) {
			ctx.drawImage(im.io_panel, 0, 0, L.consoleW, L.ioH);
			for (const l of L.iolamps) this.drawLamp(l, 0);
		}
		ctx.drawImage(im.pdp1_panel_2x, 0, this.ioOff, L.consoleW, L.consoleH);
		for (const l of L.lamps) this.drawLamp(l, this.ioOff);

		for (const s of L.switches) {
			const on = (this.sw[s.word] & s.mask) !== 0;
			const sprite = s.horiz ? (on ? im.switch_l : im.switch_r) : (on ? im.switch_u : im.switch_d);
			ctx.drawImage(sprite, s.r[0], s.r[1] + this.ioOff, s.r[2], s.r[3]);
		}
		for (const k of L.keys) {
			const sw2 = this.sw[2];
			const sprite = (k.down && (sw2 & k.down)) ? im.key_d : ((k.up && (sw2 & k.up)) ? im.key_u : im.key_n);
			ctx.drawImage(sprite, k.r[0], k.r[1] + this.ioOff, k.r[2], k.r[3]);
		}

		if (!this.present) {
			ctx.fillStyle = 'rgba(230, 230, 230, 0.6)';
			ctx.fillRect(0, 0, L.consoleW, L.consoleH + this.ioOff);
		}
	}

	drawLamp(l, yOff) {
		const [x, y, w, h] = l.r;
		this.ctx.drawImage(this.lit[l.index] ? this.img.lamp_on : this.img.lamp_off, x, y + yOff, w, h);
	}

	// The element under a mouse event, console coordinates, or null.
	hit(e) {
		const L = this.layout, rect = this.canvas.getBoundingClientRect();
		const x = (e.clientX - rect.left) * L.consoleW / rect.width;
		const y = (e.clientY - rect.top) * (L.consoleH + this.ioOff) / rect.height - this.ioOff;
		const inside = (r) => x >= r[0] && x < r[0] + r[2] && y >= r[1] && y < r[1] + r[3];
		for (const s of L.switches) if (inside(s.r)) return { kind: 'switch', el: s };
		for (const k of L.keys) if (inside(k.r)) return { kind: 'key', el: k };
		return null;
	}

	mouseDown(e) {
		e.preventDefault();     // no text selection, no middle-button scrolling
		if (!this.layout || !this.present || this.mirror) return;
		const h = this.hit(e);
		if (!h) return;

		const want = this.sw.slice(0, 3);
		if (h.kind === 'switch') {
			const s = h.el, on = (this.sw[s.word] & s.mask) !== 0;
			const set = e.button === 0 ? !on : e.button === 1;
			want[s.word] = set ? (want[s.word] | s.mask) : (want[s.word] & ~s.mask);
		} else {
			const bit = e.button === 0 ? h.el.down : (e.button === 2 ? h.el.up : 0);
			if (!bit) return;
			want[2] |= bit;
			this.heldKey = { word: 2, mask: bit };
		}
		this.change(want);
	}

	// The base always has the key down, so the release is sent even if the screen shows it up already.
	releaseKey() {
		if (!this.heldKey) return;
		const { word, mask } = this.heldKey;
		this.heldKey = null;
		const base = this.sw.slice(0, 3), want = base.slice();
		base[word] |= mask;
		want[word] &= ~mask;
		this.change(want, base);
	}

	// Ask the server for the change, sending the base, by default what was shown, so it changes only
	// the bits that differ, and show it at once; the server's messages confirm it after swHoldMs.
	change(want, base = this.sw.slice(0, 3)) {
		if (want.every((v, i) => v === base[i])) return;
		this.send({ type: 'panel_sw', sw: want, base: base });
		for (let i = 0; i < 3; i++) this.sw[i] = want[i];
		this.swHoldUntil = performance.now() + swHoldMs;
		this.dirty = true;
	}
}
