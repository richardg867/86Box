#include <cstdint>

class QWindow;
class QWidget;
void wl_mouse_capture(QWindow *window);
void wl_mouse_uncapture();
void wl_mouse_motion(double x, double y, uint32_t time);
void wl_mouse_update_region();
void wl_set_capture_widget(QWidget *widget);
void wl_init();
