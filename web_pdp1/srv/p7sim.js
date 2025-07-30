class p7sim {
	constructor(canvas) {
		this.canvas = canvas;
		this.initGL();

		this.time = 0;
		this.points = [];
		this.newpoints = [];
//		this.indices = Array(1024*1024).fill(-1);

		this.frameInterval = 33333; // ~30 FPS

		this.pointSize = 2.0;

		// for point processing
		this.esc = false;
	}

	initGL() {
		this.gl = this.canvas.getContext('webgl', {
			alpha: false,
			premultipliedAlpha: false,
			antialias: true,
			preserveDrawingBuffer: true
		});

		if(!this.gl)
			throw new Error("WebGL not supported");

		this.initShaders();
		this.initFramebuffers();
		this.pointBuffer = this.gl.createBuffer();

		this.gl.clearColor(0, 0, 0, 1);
		this.gl.clear(this.gl.COLOR_BUFFER_BIT);
	}

	initShaders() {
		const point_vs = `
attribute vec4 in_pos;
varying float v_intensity;
varying float v_fade;
uniform float u_pointSize;
void main() {
	v_fade = pow(0.5, in_pos.z);
	float sz = mix(0.0018, 0.0055, in_pos.w)*1024.0/2.0;
	v_intensity = mix(0.25, 1.0, in_pos.w);
	gl_Position = vec4((in_pos.xy / 512.0) - 1.0, 0, 1);
	gl_PointSize = u_pointSize*sz;
}
`;

		const point_fs = `
precision mediump float;
varying float v_intensity;
varying float v_fade;
void main() {
	float dist = length(2.0*gl_PointCoord - vec2(1));
	float intens = clamp(1.0 - dist*dist, 0.0, 1.0)*v_intensity;

	vec4 color = vec4(0);
	color.x = intens*v_fade;
	color.y = intens;
	color.z = 1.0;
	gl_FragColor = color;
}
`;


		const vs = `
attribute vec2 in_pos;
attribute vec2 in_uv;
varying vec2 v_uv;
void main() {
	v_uv = in_uv;
	gl_Position = vec4(in_pos, 0, 1);
}
`;

		const excite_fs = `
precision mediump float;
varying vec2 v_uv;
uniform sampler2D tex0;
uniform sampler2D tex1;
void main() {
	vec4 white = texture2D(tex0, v_uv);
	vec4 yellow = texture2D(tex1, v_uv);
	vec4 color = max(vec4(white.y*white.z), 0.987*yellow);
	color = floor(color*255.0)/255.0;
	gl_FragColor = color;
}
`;

		const combine_fs =
`
precision mediump float;
varying vec2 v_uv;
uniform sampler2D tex0;
uniform sampler2D tex1;
void main() {
	vec4 bphos1 = vec4(0.24, 0.667, 0.969, 1.0);
	vec4 yphos1 = 0.9*vec4(0.475, 0.8, 0.243, 1.0);
	vec4 yphos2 = 0.975*vec4(0.494, 0.729, 0.118, 0.0);

	vec2 uv = vec2(v_uv.x, v_uv.y);
	vec4 white = texture2D(tex0, uv);
	vec4 yellow = texture2D(tex1, uv);
	vec4 yel = mix(yphos2, yphos1, yellow.x);
	float a = 0.663 * (yel.a + (1.0-cos(3.141569*yel.a))/2.0)/2.0;
	gl_FragColor = bphos1*white.x*white.z + yel*a;
}
`;

		let self = this;
		function progAndLocs(vs, fs, attribs, uniforms) {
			let prog = self.createProgram(vs, fs);
			let locs = {};
			for(const a of attribs)
				locs[a] = self.gl.getAttribLocation(prog, a);
			for(const u of uniforms)
				locs[u] = self.gl.getUniformLocation(prog, u);
			return { prog: prog, locs: locs };
		};

		this.pointProg = progAndLocs(point_vs, point_fs, ["in_pos"], ["u_pointSize"]);
		this.exciteProg = progAndLocs(vs, excite_fs, ["in_pos", "in_uv"], ["tex0", "tex1"]);
		this.combineProg = progAndLocs(vs, combine_fs, ["in_pos", "in_uv"], ["tex0", "tex1"]);

		this.createQuadBuffers();
	}

	/**
	 * Create a shader from source
	 */
	createShader(type, source) {
		const shader = this.gl.createShader(type);
		this.gl.shaderSource(shader, source);
		this.gl.compileShader(shader);

		if (!this.gl.getShaderParameter(shader, this.gl.COMPILE_STATUS)) {
			const info = this.gl.getShaderInfoLog(shader);
			throw new Error('Could not compile WebGL shader: ' + info);
		}

		return shader;
	}

	/**
	 * Create a program from vertex and fragment shader sources
	 */
	createProgram(vertexShaderSource, fragmentShaderSource) {
		const vertexShader = this.createShader(this.gl.VERTEX_SHADER, vertexShaderSource);
		const fragmentShader = this.createShader(this.gl.FRAGMENT_SHADER, fragmentShaderSource);

		const program = this.gl.createProgram();
		this.gl.attachShader(program, vertexShader);
		this.gl.attachShader(program, fragmentShader);
		this.gl.linkProgram(program);

		if (!this.gl.getProgramParameter(program, this.gl.LINK_STATUS)) {
			const info = this.gl.getProgramInfoLog(program);
			throw new Error('Could not link WebGL program: ' + info);
		}

		return program;
	}

	initFramebuffers() {
		const gl = this.gl;
		// internal FBs better be big enough
		const [w, h] = [1024, 1024];
		this.whiteFBO = this.createFramebuffer(w, h);
		this.yellowFBO = [ this.createFramebuffer(w, h), this.createFramebuffer(w, h) ];
		this.flip = 0;
	}

	createFramebuffer(width, height) {
		const gl = this.gl;

		const texture = gl.createTexture();
		gl.bindTexture(gl.TEXTURE_2D, texture);
		gl.texImage2D(
			gl.TEXTURE_2D, 0, gl.RGBA,
			width, height, 0,
			gl.RGBA, gl.UNSIGNED_BYTE, null
		);

		gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
		gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
		gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
		gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);

		const framebuffer = gl.createFramebuffer();
		gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);

		gl.framebufferTexture2D(
			gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0,
			gl.TEXTURE_2D, texture, 0
		);

		if (gl.checkFramebufferStatus(gl.FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE)
			throw new Error('Framebuffer is not complete');

		gl.bindFramebuffer(gl.FRAMEBUFFER, null);

		return {
			framebuffer: framebuffer,
			texture: texture
		};
	}

	/**
	 * Create buffer for fullscreen quad
	 */
	createQuadBuffers() {
		const gl = this.gl;

		// Create buffer for quad vertices
		this.quadBuffer = gl.createBuffer();
		gl.bindBuffer(gl.ARRAY_BUFFER, this.quadBuffer);

		// Define quad vertices (2 triangles covering the screen)
		const quadVertices = new Float32Array([
			// positions (x,y)	// texture coords (s,t)
			-1.0,  1.0,		   0.0, 1.0,	// top left
			-1.0, -1.0,		   0.0, 0.0,	// bottom left
			 1.0,  1.0,		   1.0, 1.0,	// top right

			 1.0,  1.0,		   1.0, 1.0,	// top right
			-1.0, -1.0,		   0.0, 0.0,	// bottom left
			 1.0, -1.0,		   1.0, 0.0	 // bottom right
		]);

		gl.bufferData(gl.ARRAY_BUFFER, quadVertices, gl.STATIC_DRAW);
	}

	processIncoming(data) {
		for (let i = 0; i < data.length; i++) {
			const cmd = data[i];
			const dt = (cmd >> 23) & 0o777;

			// escape for longer delays of nothing
			if(this.esc) {
				this.esc = false;
				this.time += cmd;
			} else if(dt == 511) {
				this.esc = true;
			} else {
				const x = cmd & 0o1777;
				const y = (cmd >> 10) & 0o1777;
				const intensity = (cmd >> 20) & 7;

				this.time += dt;

				if(x != 0 || y != 0)
					this.newpoints.push({
						x: x,
						y: y,
						intensity: intensity,
						age: this.frameInterval - this.time
					});
			}

			while(this.time >= this.frameInterval) {
				this.time -= this.frameInterval;
				this.process();
				this.draw();
			}
		}
	}

	process() {
		// TODO?: indices

		let points = [];
		for(let p of this.points) {
			p.age += this.frameInterval;
			if(p.age < 200000)
				points.push(p);
		}
		this.points = points;

		this.points.push(...this.newpoints);
		this.newpoints = [];
	}

	draw() {
		const gl = this.gl;

		gl.viewport(0, 0, 1024, 1024);
		this.drawWhite(this.whiteFBO.framebuffer);

		gl.disable(gl.BLEND);
		this.composePass(
			this.yellowFBO[this.flip].framebuffer,
			this.whiteFBO.texture, 
			this.yellowFBO[1-this.flip].texture,
			this.exciteProg);

		gl.viewport(0, 0, gl.drawingBufferWidth, gl.drawingBufferHeight);
		this.composePass(
			null,
			this.whiteFBO.texture, 
			this.yellowFBO[this.flip].texture,
			this.combineProg);

		this.flip = 1 - this.flip;
	}

	drawWhite(fbo) {
		const gl = this.gl;

		const prog = this.pointProg.prog;
		const locs = this.pointProg.locs;

		gl.enable(gl.BLEND);
		gl.blendFunc(gl.ONE, gl.ONE);

		gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
		gl.clearColor(0, 0, 0, 1);
		gl.clear(gl.COLOR_BUFFER_BIT);
		gl.useProgram(prog);

		gl.uniform1f(locs.u_pointSize, this.pointSize);

		const nverts = this.points.length;

		const positions = new Float32Array(nverts * 4);
		for (let i = 0; i < nverts; i++) {
			const point = this.points[i];
			positions[i*4 + 0] = point.x;
			positions[i*4 + 1] = point.y;
			positions[i*4 + 2] = point.age / 50000.0;
			positions[i*4 + 3] = point.intensity/7.0;
		}

		gl.bindBuffer(gl.ARRAY_BUFFER, this.pointBuffer);
		gl.bufferData(gl.ARRAY_BUFFER, positions, gl.DYNAMIC_DRAW);

		gl.enableVertexAttribArray(locs.in_pos);
		gl.vertexAttribPointer(locs.in_pos, 4, gl.FLOAT, false, 0, 0);

		gl.drawArrays(gl.POINTS, 0, nverts);
	}

	composePass(fbo, tex0, tex1, prog) {
		const gl = this.gl;

		const locs = prog.locs;

		gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
		gl.clear(gl.COLOR_BUFFER_BIT);
		gl.useProgram(prog.prog);

		gl.activeTexture(gl.TEXTURE0);
		gl.bindTexture(gl.TEXTURE_2D, tex0);
		gl.uniform1i(locs.tex0, 0);
		gl.activeTexture(gl.TEXTURE1);
		gl.bindTexture(gl.TEXTURE_2D, tex1);
		gl.uniform1i(locs.tex1, 1);

		gl.bindBuffer(gl.ARRAY_BUFFER, this.quadBuffer);
		gl.enableVertexAttribArray(locs.in_pos);
		gl.vertexAttribPointer(locs.in_pos, 2, gl.FLOAT, false, 16, 0);
		gl.enableVertexAttribArray(locs.in_uv);
		gl.vertexAttribPointer(locs.in_uv, 2, gl.FLOAT, false, 16, 8);

		gl.drawArrays(gl.TRIANGLES, 0, 6);
	}

	updateSettings(settings) {
		if(settings.pointSize !== undefined)
			this.pointSize = settings.pointSize;
	}
}
