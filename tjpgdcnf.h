/*----------------------------------------------*/
/* TJpgDec System Configurations for SF2000     */
/*----------------------------------------------*/

#define	JD_SZBUF		512
/* Specifies size of stream input buffer */

#define JD_FORMAT		1
/* Output pixel format: RGB565 (16-bit/pix) */

#define	JD_USE_SCALE	1
/* Enable output descaling */

#define JD_TBLCLIP		1
/* Use table conversion for saturation arithmetic */

#define JD_FASTDECODE	1
/* Optimization for 32-bit MCUs (MIPS32) */
