const int = (x) => x | 0;

class Papertape {
	constructor(readerCanvas, punchCanvas) {
		this.ptrbuf = null;
		this.ptpbuf = [];
		this.ptrpos = 0;
		this.needsRedraw = true;
		this.lastDrawTime = 0;
		this.readerCanvas = readerCanvas;
		this.punchCanvas = punchCanvas;

		addEventListener("resize", (event) => { this.needsRedraw = true; })
	}

	setPos(pos) {
		this.ptrpos = pos;
		this.needsRedraw = true;
	}

	punchData(data) {
		this.ptpbuf.push(data);
		this.needsRedraw = true;
	}

	setReader(data) {
		this.ptrbuf = data;
		this.ptrpos = 0;
		this.needsRedraw = true;
	}

	clearPunch() {
		this.ptpbuf = [];
		this.needsRedraw = true;
	}

	draw() {
		const currentTime = Date.now();
		if (currentTime - this.lastDrawTime < 33) return; // 30fps limit
		this.lastDrawTime = currentTime;

		if (!this.needsRedraw) return;
		this.needsRedraw = false;

		this.drawReaderTape();
		this.drawPunchTape();
	}

	sizes(canvas) {
		let space = int(canvas.height/10);
		if(space < 4) space = 4;
		let r = space/2 - 1;
		let fr = r/2 + 1;
		if(fr >= r-2) fr -= 1;
		return [space, r, fr];
	}

	drawReaderTape() {
		const canvas = this.readerCanvas;
		const ctx = canvas.getContext('2d');

		// WTF
		canvas.width = canvas.clientWidth;
		canvas.height = canvas.clientHeight;

		ctx.fillStyle = '#505050';
		let [width, height] = [canvas.width, canvas.height];
		ctx.fillRect(0, 0, width, height);

		if(!this.ptrbuf) return;

		let [space, r, fr] = this.sizes(canvas);

		let i = this.ptrpos - 10;
		let readpos = 10*space;

		// tape
		ctx.fillStyle = '#FFFFB0';
		let x = i < 0 ? -i*space : 0;
		let w = (this.ptrbuf.length-i)*space - x;
		if(w > width) w = width;
		ctx.fillRect(x, 0, width, height);

		// reader indicator
		ctx.strokeStyle = '#ffffff';
		ctx.lineWidth = 2;
		ctx.beginPath();
		ctx.moveTo(readpos, 0);
		ctx.lineTo(readpos, height);
		ctx.stroke();

		ctx.fillStyle = '#000000';
		for(let x = int(space/2); x < width+2*r; x += space, i++) {
			let c;
			if(i >= 0 && i < this.ptrbuf.length)
				c = this.ptrbuf[i];
			else
				continue;

			if(c & 0x01) this.drawHole(ctx, x, space, r);
			if(c & 0x02) this.drawHole(ctx, x, space * 2, r);
			if(c & 0x04) this.drawHole(ctx, x, space * 3, r);
			this.drawHole(ctx, x, space * 4, fr);
			if(c & 0x08) this.drawHole(ctx, x, space * 5, r);
			if(c & 0x10) this.drawHole(ctx, x, space * 6, r);
			if(c & 0x20) this.drawHole(ctx, x, space * 7, r);
			if(c & 0x40) this.drawHole(ctx, x, space * 8, r);
			if(c & 0x80) this.drawHole(ctx, x, space * 9, r);
		}
	}

	drawPunchTape() {
		const canvas = this.punchCanvas;
		const ctx = canvas.getContext('2d');

		// WTF
		canvas.width = canvas.clientWidth;
		canvas.height = canvas.clientHeight;

		ctx.fillStyle = '#505050';
		let [width, height] = [canvas.width, canvas.height];
		ctx.fillRect(0, 0, width, height);

		let [space, r, fr] = this.sizes(canvas);

		let punchpos = width - 5*space;

		// tape
		ctx.fillStyle = '#FFFFB0';
		let x = punchpos - this.ptpbuf.length*space;
		if(x < 0) x = 0;
		ctx.fillRect(x, 0, width, height);

		// punch indicator
		ctx.strokeStyle = '#ffffff';
		ctx.lineWidth = 2;
		ctx.beginPath();
		ctx.moveTo(punchpos, 0);
		ctx.lineTo(punchpos, height);
		ctx.stroke();

		ctx.fillStyle = '#000000';
		let i = this.ptpbuf.length-1;
		for(let x = punchpos - int(space/2); x > -2*r; x -= space) {
			if(i < 0) break;
			let c = this.ptpbuf[i--];

			if(c & 0x01) this.drawHole(ctx, x, space, r);
			if(c & 0x02) this.drawHole(ctx, x, space * 2, r);
			if(c & 0x04) this.drawHole(ctx, x, space * 3, r);
			this.drawHole(ctx, x, space * 4, fr);
			if(c & 0x08) this.drawHole(ctx, x, space * 5, r);
			if(c & 0x10) this.drawHole(ctx, x, space * 6, r);
			if(c & 0x20) this.drawHole(ctx, x, space * 7, r);
			if(c & 0x40) this.drawHole(ctx, x, space * 8, r);
			if(c & 0x80) this.drawHole(ctx, x, space * 9, r);
		}
	}

	drawHole(ctx, x, y, size) {
		ctx.beginPath();
		ctx.arc(x, y, size, 0, Math.PI * 2);
		ctx.fill();
	}
}
