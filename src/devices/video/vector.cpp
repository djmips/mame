// license:BSD-3-Clause
// copyright-holders:Brad Oliver,Aaron Giles,Bernd Wiebelt,Allard van der Bas
/******************************************************************************
 *
 * vector.c
 *
 *        anti-alias code by Andrew Caldwell
 *        (still more to add)
 *
 * 040227 Fixed miny clip scaling which was breaking in mhavoc. AREK
 * 010903 added support for direct RGB modes MLR
 * 980611 use translucent vectors. Thanks to Peter Hirschberg
 *        and Neil Bradley for the inspiration. BW
 * 980307 added cleverer dirty handling. BW, ASG
 *        fixed antialias table .ac
 * 980221 rewrote anti-alias line draw routine
 *        added inline assembly multiply fuction for 8086 based machines
 *        beam diameter added to draw routine
 *        beam diameter is accurate in anti-alias line draw (Tcosin)
 *        flicker added .ac
 * 980203 moved LBO's routines for drawing into a buffer of vertices
 *        from avgdvg.c to this location. Scaling is now initialized
 *        by calling vector_init(...). BW
 * 980202 moved out of msdos.c ASG
 * 980124 added anti-alias line draw routine
 *        modified avgdvg.c and sega.c to support new line draw routine
 *        added two new tables Tinten and Tmerge (for 256 color support)
 *        added find_color routine to build above tables .ac
 *
 * Vector Team
 *
 *        Brad Oliver
 *        Aaron Giles
 *        Bernd Wiebelt
 *        Allard van der Bas
 *        Al Kossow (VECSIM)
 *        Hedley Rainnie (VECSIM)
 *        Eric Smith (VECSIM)
 *        Neil Bradley (technical advice)
 *        Andrew Caldwell (anti-aliasing)
 *
 **************************************************************************** */

#include "emu.h"
#include "emuopts.h"
#include "rendutil.h"
#include "vector.h"

// Serial port related includes
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
//#include <termios.h>
#include <errno.h>

#include <inttypes.h>
#include <sys/time.h>


// hack
#include <windows.h> 
static HANDLE hSerial;
 

#define VECTOR_SERIAL_MAX 4095

#define VECTOR_WIDTH_DENOM 512

#define MAX_POINTS 10000

float vector_options::s_flicker = 0.0f;
float vector_options::s_beam_width_min = 0.0f;
float vector_options::s_beam_width_max = 0.0f;
float vector_options::s_beam_intensity_weight = 0.0f;
const char *vector_options::s_serial = (const char *)" ";
float vector_options::s_serial_scale_x = 0.0f;
float vector_options::s_serial_scale_y = 1.0f;
float vector_options::s_serial_offset_x = 1.0f;
float vector_options::s_serial_offset_y = 0.0f;
int vector_options::s_serial_rotate = 0;
int vector_options::s_serial_bright = 0;
int vector_options::s_serial_drop_frame = 0;
int vector_options::s_serial_sort = 0;

void vector_options::init(emu_options& options)
{
	s_beam_width_min = options.beam_width_min();
	s_beam_width_max = options.beam_width_max();
	s_beam_intensity_weight = options.beam_intensity_weight();
	s_flicker = options.flicker();

	/* Setup the serial output of the XY coords if configured */
	s_serial = options.vector_serial();
  	const float scale = options.vector_scale();
	if (scale != 0.0)
	{
		// user specified a scale on the command line
		s_serial_scale_x = s_serial_scale_y = scale;
	} else {
		// use the per-axis scales
		s_serial_scale_x = options.vector_scale_x();
		s_serial_scale_y = options.vector_scale_y();
	}

	s_serial_offset_x = options.vector_offset_x();
	s_serial_offset_y = options.vector_offset_y();
	s_serial_rotate = options.vector_rotate();
	s_serial_bright = options.vector_bright();
	s_serial_drop_frame = 0;
	s_serial_sort = 1;
}

// device type definition
DEFINE_DEVICE_TYPE(VECTOR, vector_device, "vector_device", "VECTOR")

vector_device::vector_device(const machine_config &mconfig, const char *tag, device_t *owner, uint32_t clock)
	: device_t(mconfig, VECTOR, tag, owner, clock),
		device_video_interface(mconfig, *this),
		m_vector_list(nullptr),
		m_min_intensity(255),
		m_max_intensity(0)
{
}

struct serial_segment_t {
	struct serial_segment_t * next;
	int x0;
	int y0;
	int x1;
	int y1;
	rgb_t argb;

	serial_segment_t(
		int x0,
		int y0,
		int x1,
		int y1,
		rgb_t argb
	) :
		next(NULL),
		x0(x0),
		y0(y0),
		x1(x1),
		y1(y1),
		argb(argb)
	{
	}
};

int
serial_open(
        const char * const dev
)
{

#ifdef OSX	
        const int fd = open(dev, O_RDWR | O_NONBLOCK | O_NOCTTY, 0666);
        if (fd < 0)
                return -1;

        // Disable modem control signals
        struct termios attr;
        tcgetattr(fd, &attr);
        attr.c_cflag |= CLOCAL | CREAD;
        attr.c_oflag &= ~OPOST;
        tcsetattr(fd, TCSANOW, &attr);
#endif

		// Hack hack Windows code

		std::string deviceRoot = std::string("\\.\\");
		std::string device = std::string(dev);
		deviceRoot += device;

		hSerial = CreateFile(deviceRoot.c_str(),
				GENERIC_READ | GENERIC_WRITE,
				0, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);

		DCB options = {0};
		COMMTIMEOUTS timeouts = {0};

		if(hSerial == INVALID_HANDLE_VALUE) goto error;

		options.DCBlength=sizeof(options);

		if(!GetCommState(hSerial, &options)) goto error;

		options.BaudRate = 500000;
		options.ByteSize = 8;
		options.StopBits = ONESTOPBIT;
		options.Parity   = NOPARITY;

		if(!SetCommState(hSerial, &options)) goto error;

		timeouts.ReadIntervalTimeout = 50;
		timeouts.ReadTotalTimeoutConstant = 50;
		timeouts.ReadTotalTimeoutMultiplier = 10;
		timeouts.WriteTotalTimeoutConstant = 50;
		timeouts.WriteTotalTimeoutMultiplier = 10;

		if(!SetCommTimeouts(hSerial, &timeouts)) goto error;


		return 1;

 error:  
  //SET_ERROR(XLINK_ERROR_FILE, strerror(GetLastError()));
	return -1;
 }

// void vector_device::serial_draw_point(
// 	unsigned x,
// 	unsigned y,
// 	rgb_t argb
// )
// {
// 	// make sure that we are in range; should always be
// 	// due to clipping on the window, but just in case
// 	if (x < 0) x = 0;
// 	if (y < 0) y = 0;

// 	if (x > VECTOR_SERIAL_MAX) x = VECTOR_SERIAL_MAX;
// 	if (y > VECTOR_SERIAL_MAX) y = VECTOR_SERIAL_MAX;

// 	// always flip the Y, since the vectorscope measures
// 	// 0,0 at the bottom left corner, but this coord uses
// 	// the top left corner.
// 	//y = VECTOR_SERIAL_MAX - y;

// 	int intensity = argb>>24 & 0xFF;

// 	unsigned bright;
// 	if (intensity > vector_options::s_serial_bright)
// 		bright = 63;
// 	else
// 	if (intensity <= 0)
// 		bright = 0;
// 	else
// 		bright = (intensity * 64) / 256;

// 	if (bright > 63)
// 		bright = 63;

// 	if (vector_options::s_serial_rotate == 1)
// 	{
// 		// +90
// 		unsigned tmp = x;
// 		x = VECTOR_SERIAL_MAX - y;
// 		y = tmp;
// 	} else
// 	if (vector_options::s_serial_rotate == 2)
// 	{
// 		// +180
// 		x = VECTOR_SERIAL_MAX - x;
// 		y = VECTOR_SERIAL_MAX - y;
// 	} else
// 	if (vector_options::s_serial_rotate == 3)
// 	{
// 		// -90
// 		unsigned t = x;
// 		x = y;
// 		y = VECTOR_SERIAL_MAX - t;
// 	}

// 	uint32_t cmd = 0
// 		| (2 << 30)
// 		| (bright & 0x3F) << 24
// 		| (x & 0xFFF) << 12
// 		| (y & 0xFFF) <<  0
// 		;

// 	//printf("%08x %8d %8d %3d\n", cmd, x, y, intensity);

// 	m_serial_buf[m_serial_offset++] = cmd >> 24;
// 	m_serial_buf[m_serial_offset++] = cmd >> 16;
// 	m_serial_buf[m_serial_offset++] = cmd >>  8;
// 	m_serial_buf[m_serial_offset++] = cmd >>  0;

// 	// todo: check for overflow;
// 	// should always have enough points
// }


void vector_device::serial_draw_point(
	unsigned x,
	unsigned y,
	rgb_t argb
)
{
	// make sure that we are in range; should always be
	// due to clipping on the window, but just in case
	// if (x < 0) x = 0;  // Doesn't make sense on an unsigned var
	// if (y < 0) y = 0;

	if (x > VECTOR_SERIAL_MAX) x = VECTOR_SERIAL_MAX;
	if (y > VECTOR_SERIAL_MAX) y = VECTOR_SERIAL_MAX;

	// always flip the Y, since the vectorscope measures
	// 0,0 at the bottom left corner, but this coord uses
	// the top left corner.
	//y = VECTOR_SERIAL_MAX - y;

	x = x >> 2;
	y = y >> 2;

	unsigned rgb12 = ((argb.r() >> 4) << 8) | ((argb.g() >> 4) << 4) | ((argb.b() >> 4) << 0);

	uint32_t cmd = 0
		| (rgb12 & 0xFFF) << 20
		| (x & 0x3FF) << 10
		| (y & 0x3FF) <<  0
		;

	//printf("%08x %8d %8d %03x\n", cmd, x, y, rgb12);

	m_serial_buf[m_serial_offset++] = cmd >> 24;
	m_serial_buf[m_serial_offset++] = cmd >> 16;
	m_serial_buf[m_serial_offset++] = cmd >>  8;
	m_serial_buf[m_serial_offset++] = cmd >>  0;

	// todo: check for overflow;
	// should always have enough points
}

// This will only be called with non-zero intensity lines.
// we keep a linked list of the vectors and sort them with
// a greedy insertion sort.
void vector_device::serial_draw_line(
	float xf0,
	float yf0,
	float xf1,
	float yf1,
	rgb_t argb
)
{
	if (m_serial_fd < 0)
		return;

	// scale and shift each of the axes.
	const int x0 = (xf0 * VECTOR_SERIAL_MAX - VECTOR_SERIAL_MAX/2) * vector_options::s_serial_scale_x + vector_options::s_serial_offset_x;
	const int y0 = (yf0 * VECTOR_SERIAL_MAX - VECTOR_SERIAL_MAX/2) * vector_options::s_serial_scale_y + vector_options::s_serial_offset_y;
	const int x1 = (xf1 * VECTOR_SERIAL_MAX - VECTOR_SERIAL_MAX/2) * vector_options::s_serial_scale_x + vector_options::s_serial_offset_x;
	const int y1 = (yf1 * VECTOR_SERIAL_MAX - VECTOR_SERIAL_MAX/2) * vector_options::s_serial_scale_y + vector_options::s_serial_offset_y;

	serial_segment_t * const new_segment
		= new serial_segment_t(x0, y0, x1, y1, argb);

	if (this->m_serial_segments_tail)
		this->m_serial_segments_tail->next = new_segment;
	else
		this->m_serial_segments = new_segment;

	this->m_serial_segments_tail = new_segment;
}


void vector_device::serial_reset()
{
	m_serial_offset = 0;
	m_serial_buf[m_serial_offset++] = 0;
	m_serial_buf[m_serial_offset++] = 0;
	m_serial_buf[m_serial_offset++] = 0;
	m_serial_buf[m_serial_offset++] = 0;
	m_serial_buf[m_serial_offset++] = 0;
	m_serial_buf[m_serial_offset++] = 0;
	m_serial_buf[m_serial_offset++] = 0;
	m_serial_buf[m_serial_offset++] = 0;


	// end sync
	m_serial_buf[m_serial_offset++] = 0xAA;


	m_vector_transit[0] = 0;
	m_vector_transit[1] = 0;
	m_vector_transit[2] = 0;
}


void vector_device::serial_send()
{
	if (m_serial_fd < 0)
		return;

	int last_x = -1;
	int last_y = -1;

	// find the next closest point to the last one.
	// greedy sorting algorithm reduces beam transit time
	// fairly significantly. doesn't matter for the
	// vectorscope, but makes a big difference for Vectrex
	// and other slower displays.
	while(this->m_serial_segments)
	{
		int reverse = 0;
		int min = 1e6;
		serial_segment_t ** min_seg
			= &this->m_serial_segments;

		if (vector_options::s_serial_sort)
		for(serial_segment_t ** s = min_seg ; *s ; s = &(*s)->next)
		{
			int dx0 = (*s)->x0 - last_x;
			int dy0 = (*s)->y0 - last_y;
			int dx1 = (*s)->x1 - last_x;
			int dy1 = (*s)->y1 - last_y;
			int d0 = sqrt(dx0*dx0 + dy0*dy0);
			int d1 = sqrt(dx1*dx1 + dy1*dy1);

			if(d0 < min)
			{
				min_seg = s;
				min = d0;
				reverse = 0;
			}

			if (d1 < min)
			{
				min_seg = s;
				min = d1;
				reverse = 1;
			}

			// if we have hit two identical points,
			// then stop the search here.
			if (min == 0)
				break;
		}

		serial_segment_t * const s = *min_seg;
		if (!s)
			break;
	
		const int x0 = reverse ? s->x1 : s->x0;
		const int y0 = reverse ? s->y1 : s->y0;
		const int x1 = reverse ? s->x0 : s->x1;
		const int y1 = reverse ? s->y0 : s->y1;

		// if this is not a continuous segment,
		// we must add a transit command
		if (last_x != x0 || last_y != y0)
		{
			serial_draw_point(x0, y0, 0);
			int dx = x0 - last_x;
			int dy = y0 - last_y;
			m_vector_transit[0] += sqrt(dx*dx + dy*dy);
		}

		// transit to the new point
		int dx = x1 - x0;
		int dy = y1 - y0;
		int dist = sqrt(dx*dx + dy*dy);

		serial_draw_point(x1, y1, s->argb);
		last_x = x1;
		last_y = y1;

		if (s->argb>>24 > vector_options::s_serial_bright)
			m_vector_transit[2] += dist;
		else
			m_vector_transit[1] += dist;

		// delete this segment from the list
		*min_seg = s->next;
		delete s;
	}

	// ensure that we erase our tracks
	if(this->m_serial_segments != NULL)
		fprintf(stderr, "errr?\n");
	this->m_serial_segments = NULL;
	this->m_serial_segments_tail = NULL;

	// add the "done" command to the message
	m_serial_buf[m_serial_offset++] = 1;
	m_serial_buf[m_serial_offset++] = 1;
	m_serial_buf[m_serial_offset++] = 1;
	m_serial_buf[m_serial_offset++] = 1;

	size_t offset = 0;

	if(0)
	printf("%zu vectors: off=%u on=%u bright=%u%s\n",
		m_serial_offset/4,
		m_vector_transit[0],
		m_vector_transit[1],
		m_vector_transit[2],
		vector_options::s_serial_drop_frame ? " !" : ""
	);

	static unsigned skip_frame;
	unsigned eagain = 0;

	if (vector_options::s_serial_drop_frame || skip_frame++ % 2 != 0)
	{
		//printf("We skipped a serial frame!\n");
		// we skipped a frame, don't skip the next one
		vector_options::s_serial_drop_frame = 0;
	} else
	while (offset < m_serial_offset)
	{
		size_t wlen = m_serial_offset - offset;
		if (wlen > 64)
			wlen = 64;


		DWORD bytesWritten;
		WriteFile(hSerial, m_serial_buf + offset, m_serial_offset - offset, &bytesWritten, NULL);
		FlushFileBuffers(hSerial);

		if ( bytesWritten <= 0)
		{
			printf("Closing Serial port\n");
			// This Windows serial code is not async so it can't fail with EAGAIN
			// and then retry so it will fail immediately which could be a problem
			// Look at WriteFileEx for async version possibly.
			CloseHandle(hSerial);
			m_serial_fd = -1;
			break;
		}


		//ssize_t rc = write(m_serial_fd, m_serial_buf + offset, m_serial_offset - offset);
		// if (rc <= 0)
		// {
		// 	eagain++;
		// 	if (errno == EAGAIN)
		// 		continue;
		// 	perror(vector_options::s_serial);
		// 	close(m_serial_fd);
		// 	m_serial_fd = -1;
		// 	break;
		// }

		//offset += rc;
		
		offset += bytesWritten;

	}

	//printf("%d eagain.\n", eagain);
	if (eagain > 20)
		vector_options::s_serial_drop_frame = 1;

	serial_reset();
}

void vector_device::serial_init()
{
	serial_reset();

	if (!vector_options::s_serial || strcmp(vector_options::s_serial,"") == 0)
	{
		fprintf(stderr, "no serial vector display configured\n");
		m_serial_fd = -2;
	} else {
		m_serial_fd = serial_open(vector_options::s_serial);
		fprintf(stderr, "serial dev='%s' fd=%d\n", vector_options::s_serial, m_serial_fd);
	}
}

void vector_device::device_start()
{
	vector_options::init(machine().options());

	m_vector_index = 0;

	/* allocate memory for tables */
	m_vector_list = make_unique_clear<point[]>(MAX_POINTS);

	m_serial_segments = m_serial_segments_tail = NULL;

	// allocate enough buffer space, although we should never use this much
	m_serial_buf = auto_alloc_array_clear(machine(), unsigned char, (MAX_POINTS+2) * 4);
	if (!m_serial_buf)
	{
		// todo: how to signal an error?
	}

	serial_init();
}

/*
 * www.dinodini.wordpress.com/2010/04/05/normalized-tunable-sigmoid-functions/
 */
float vector_device::normalized_sigmoid(float n, float k)
{
	// valid for n and k in range of -1.0 and 1.0
	return (n - n * k) / (k - fabs(n) * 2.0f * k + 1.0f);
}


/*
 * Adds a line end point to the vertices list. The vector processor emulation
 * needs to call this.
 */
void vector_device::add_point(int x, int y, rgb_t color, int intensity)
{
	point *newpoint;

//printf("%d %d: %d,%d,%d @ %d\n", x, y, color.r(), color.b(), color.g(), intensity);

	// hack for the vectrex
	// -- convert "128,128,128" @ 255 to "255,255,255" @ 127
	// if (color.r() == 128
	// &&  color.b() == 128
	// &&  color.g() == 128
	// &&  intensity == 255)
	// {
	// 	color = rgb_t(255,255,255);
	// 	intensity = 128;
	// }

	intensity = std::max(0, std::min(255, intensity));

	m_min_intensity = intensity > 0 ? std::min(m_min_intensity, intensity) : m_min_intensity;
	m_max_intensity = intensity > 0 ? std::max(m_max_intensity, intensity) : m_max_intensity;

	if (vector_options::s_flicker && (intensity > 0))
	{
		float random = (float)(machine().rand() & 255) / 255.0f; // random value between 0.0 and 1.0

		intensity -= (int)(intensity * random * vector_options::s_flicker);

		intensity = std::max(0, std::min(255, intensity));
	}

	newpoint = &m_vector_list[m_vector_index];
	newpoint->x = x;
	newpoint->y = y;
	newpoint->col = color;
	newpoint->intensity = intensity;

	m_vector_index++;
	if (m_vector_index >= MAX_POINTS)
	{
		m_vector_index--;
		logerror("*** Warning! Vector list overflow!\n");
	}
}


/*
 * The vector CPU creates a new display list. We save the old display list,
 * but only once per refresh.
 */
void vector_device::clear_list(void)
{
	m_vector_index = 0;
}


uint32_t vector_device::screen_update(screen_device &screen, bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	uint32_t flags = PRIMFLAG_ANTIALIAS(1) | PRIMFLAG_BLENDMODE(BLENDMODE_ADD) | PRIMFLAG_VECTOR(1);
	const rectangle &visarea = screen.visible_area();
	float xscale = 1.0f / (65536 * visarea.width());
	float yscale = 1.0f / (65536 * visarea.height());
	float xoffs = (float)visarea.min_x;
	float yoffs = (float)visarea.min_y;

	point *curpoint;
	int lastx = 0;
	int lasty = 0;

	curpoint = m_vector_list.get();

	screen.container().empty();
	screen.container().add_rect(0.0f, 0.0f, 1.0f, 1.0f, rgb_t(0xff,0x00,0x00,0x00), PRIMFLAG_BLENDMODE(BLENDMODE_ALPHA) | PRIMFLAG_VECTORBUF(1));

	for (int i = 0; i < m_vector_index; i++)
	{
		render_bounds coords;

		float intensity = (float)curpoint->intensity / 255.0f;
		float intensity_weight = normalized_sigmoid(intensity, vector_options::s_beam_intensity_weight);

		// check for static intensity
		float beam_width = m_min_intensity == m_max_intensity
			? vector_options::s_beam_width_min
			: vector_options::s_beam_width_min + intensity_weight * (vector_options::s_beam_width_max - vector_options::s_beam_width_min);

		// normalize width
		beam_width *= 1.0f / (float)VECTOR_WIDTH_DENOM;

		coords.x0 = ((float)lastx - xoffs) * xscale;
		coords.y0 = ((float)lasty - yoffs) * yscale;
		coords.x1 = ((float)curpoint->x - xoffs) * xscale;
		coords.y1 = ((float)curpoint->y - yoffs) * yscale;

		if (curpoint->intensity != 0)
		{
			screen.container().add_line(
				coords.x0, coords.y0, coords.x1, coords.y1,
				beam_width,
				(curpoint->intensity << 24) | (curpoint->col & 0xffffff),
				flags);

			serial_draw_line(
				coords.x0, coords.y0,
				coords.x1, coords.y1,
				(curpoint->intensity << 24) | (curpoint->col & 0xffffff));
		}

		lastx = curpoint->x;
		lasty = curpoint->y;

		curpoint++;
	}

	if (m_serial_fd == -1)
	{
		// try to restart the serial
		serial_init();
	}

	serial_send();

	return 0;
}
