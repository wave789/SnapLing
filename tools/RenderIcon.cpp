#include <QCoreApplication>
#include <QSvgRenderer>
#include <QPainter>
int main(int argc,char **argv){QCoreApplication app(argc,argv);if(argc!=3)return 2;QSvgRenderer svg(QString::fromLocal8Bit(argv[1]));if(!svg.isValid())return 3;QImage image(128,128,QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);QPainter p(&image);svg.render(&p);p.end();return image.save(QString::fromLocal8Bit(argv[2]),"PNG")?0:4;}
