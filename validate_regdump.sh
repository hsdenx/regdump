#!/bin/sh
#
# Script which validates regdump is working good

function one_test()
{
	./regdump $1 $2 > gnlmpf
	diff gnlmpf testoutputs/$3
	if [ $? -ne 0 ]; then
		echo  $1 failed
		exit 1
	fi
}

one_test soc/am1808.reg board/cmc_reg.reg am1808_reg_cmc_reg_reg
one_test soc/imx6qdl.reg board/aristainetos2_ub.reg imx6qdl_reg_aristainetos_ub_reg
one_test soc/imx25.reg board/cetec_mx25.reg imx25_reg_cetec_mx25_reg
one_test soc/kirkwood.reg board/scada2.reg kirkwood_reg_scada2_reg
one_test soc/am3517.reg board/twister.reg am3517_reg_twister_reg
one_test soc/am335x.reg board/zug_pxm2.reg am335x_reg_zug_pxm2_reg
one_test soc/sam9261.reg board/smartweb.reg sam9261_reg_smartweb_reg

#cleanup
rm gnlmpf
