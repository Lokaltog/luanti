// fx: GPU particle spawn (see the vertex shader)
flat VARYING_ vec4 vS0;
flat VARYING_ vec4 vS1;
flat VARYING_ vec4 vS2;
flat VARYING_ vec4 vS3;

void main(void)
{
	gl_FragData[0] = vS0;
	gl_FragData[1] = vS1;
	gl_FragData[2] = vS2;
	gl_FragData[3] = vS3;
}
